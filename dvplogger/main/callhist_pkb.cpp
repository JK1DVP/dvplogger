/*
 * Packed Call History cache builder for dvplogger.
 * .PCK is authoritative; .PKB is a disposable, automatically rebuilt cache.
 */
#include "Arduino.h"
#include "SD.h"
#include "decl.h"
#include "variables.h"
#include "callhist.h"
#include "callhist_mem.h"
#include "dupechk.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

namespace {
static const uint8_t PKB_VERSION = 1;
static const uint8_t PKB_HEADER_SIZE = 32;
static const char PKB_MAGIC[4] = {'D','P','K','B'};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
  while (n--) {
    crc ^= *p++;
    for (uint8_t i=0;i<8;i++) crc=(crc>>1)^(0xEDB88320UL & (uint32_t)-(int32_t)(crc&1));
  }
  return crc;
}
static void put16(uint8_t *p,uint16_t v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static uint16_t get16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}

static bool make_names(const char *src,char *pkb,size_t npkb,char *tmp,size_t ntmp) {
  if(!src||!*src) return false;
  const char *base=src[0]=='/'?src+1:src;
  const char *dot=strrchr(base,'.');
  size_t blen=dot?(size_t)(dot-base):strlen(base);
  if(blen<1||blen>8) return false;
  if(dot && strcasecmp(dot,".PCK")!=0) return false;
  snprintf(pkb,npkb,"/%.*s.PKB",(int)blen,base);
  snprintf(tmp,ntmp,"/%.*s.PBT",(int)blen,base);
  return true;
}

static int code6(char c,bool call) {
  c=(char)toupper((unsigned char)c);
  if(c>='0'&&c<='9') return 1+(c-'0');
  if(c>='A'&&c<='Z') return 11+(c-'A');
  if(call&&c=='/') return 37;
  if(call&&c=='-') return 38;
  return -1;
}

static bool split_line(char *line,char **call,char **exch) {
  char *p=line; while(*p&&isspace((unsigned char)*p)) p++;
  if(!*p) return false;
  *call=p; while(*p&&!isspace((unsigned char)*p)) p++;
  if(!*p) return false;
  *p++='\0';
  while(*p&&isspace((unsigned char)*p)) p++;
  if(!*p) return false;
  *exch=p;
  char *e=p+strlen(p); while(e>p&&isspace((unsigned char)e[-1])) *--e='\0';
  for(char *q=*call;*q;q++) if(code6(*q,true)<0) return false;
  for(char *q=*exch;*q;q++) if(code6(*q,false)<0) return false;
  return strlen(*call)<=63 && strlen(*exch)<=63;
}

static size_t pack6(const char *s,bool call,uint8_t *out,size_t cap) {
  uint32_t bits=0; unsigned nbits=0; size_t n=0;
  for(;*s;s++) {
    int c=code6(*s,call); if(c<0) return 0;
    bits=(bits<<6)|(uint32_t)c; nbits+=6;
    while(nbits>=8) { nbits-=8; if(n>=cap)return 0; out[n++]=(uint8_t)(bits>>nbits); if(nbits) bits&=((1UL<<nbits)-1); else bits=0; }
  }
  if(nbits) { if(n>=cap)return 0; out[n++]=(uint8_t)(bits<<(8-nbits)); }
  return n;
}

static bool fingerprint(const char *src,uint32_t *size,uint32_t *crc,uint32_t *lines) {
  File f=SD.open(src,FILE_READ); if(!f)return false;
  uint8_t b[512]; uint32_t c=0xFFFFFFFFUL,n=0,l=0; bool any=false,lastnl=true;
  while(f.available()) { int r=f.read(b,sizeof(b)); if(r<=0)break; c=crc32_update(c,b,r); n+=r;
    for(int i=0;i<r;i++){any=true;if(b[i]=='\n'){l++;lastnl=true;}else if(b[i]!='\r')lastnl=false;}
  }
  f.close(); if(any&&!lastnl)l++; *size=n;*crc=~c;*lines=l; return true;
}

static bool header_matches(const char *pkb,uint32_t sz,uint32_t crc) {
  File f=SD.open(pkb,FILE_READ); if(!f)return false; uint8_t h[PKB_HEADER_SIZE];
  bool ok=f.read(h,sizeof(h))==(int)sizeof(h); f.close();
  return ok && !memcmp(h,PKB_MAGIC,4) && h[4]==PKB_VERSION && h[5]==PKB_HEADER_SIZE && get32(h+8)==sz && get32(h+12)==crc;
}

static bool write_header(File &out,uint32_t srcsz,uint32_t srccrc,uint16_t recs,uint32_t datasz,uint32_t datacrc) {
  uint8_t h[PKB_HEADER_SIZE]={0}; memcpy(h,PKB_MAGIC,4);h[4]=PKB_VERSION;h[5]=PKB_HEADER_SIZE;
  put32(h+8,srcsz);put32(h+12,srccrc);put16(h+16,recs);put32(h+20,datasz);put32(h+24,datacrc);
  return out.seek(0) && out.write(h,sizeof(h))==sizeof(h);
}

#if JK1DVPLOG_HWVER != 1
static const size_t PKB_SORT_CALL_MAX = 16;
static const size_t PKB_SORT_PACKED_MAX = (PKB_SORT_CALL_MAX * 6 + 7) / 8;

struct __attribute__((packed)) PkbSortKey {
  uint32_t source_offset;
  uint8_t call_len;
  uint8_t call_packed[PKB_SORT_PACKED_MAX];
};

static int packed_call_cmp(const PkbSortKey &a,const PkbSortKey &b) {
  size_t ab=(a.call_len*6+7)/8, bb=(b.call_len*6+7)/8;
  size_t n=ab<bb?ab:bb;
  int c=memcmp(a.call_packed,b.call_packed,n);
  if(c) return c;
  if(a.call_len<b.call_len) return -1;
  if(a.call_len>b.call_len) return 1;
  if(a.source_offset<b.source_offset) return -1;
  if(a.source_offset>b.source_offset) return 1;
  return 0;
}

static int sort_key_cmp(const void *va,const void *vb) {
  return packed_call_cmp(*(const PkbSortKey*)va,*(const PkbSortKey*)vb);
}

#endif

static bool emit_record(File &out,const char *line,uint16_t *recs,uint32_t *datasz,uint32_t *datacrc) {
  char parse[128];
  strncpy(parse,line,sizeof(parse)-1); parse[sizeof(parse)-1]='\0';
  char *call,*exch;
  if(!split_line(parse,&call,&exch)) return false;
  uint8_t rec[128]; size_t cl=strlen(call),el=strlen(exch);
  rec[0]=(uint8_t)cl; rec[1]=(uint8_t)el;
  size_t nc=pack6(call,true,rec+2,sizeof(rec)-2);
  size_t ne=pack6(exch,false,rec+2+nc,sizeof(rec)-2-nc);
  if(!nc||!ne) return false;
  size_t rn=2+nc+ne;
  if(out.write(rec,rn)!=rn) return false;
  *datacrc=crc32_update(*datacrc,rec,rn); *datasz+=rn; (*recs)++;
  return true;
}

static bool build_pkb_streaming(const char *src,const char *pkb,const char *tmp,
                                uint32_t srcsz,uint32_t srccrc) {
  uint32_t t0=millis();
  size_t heap_start=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t heap_min_start=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  if(SD.exists(tmp)) SD.remove(tmp);
  File out=SD.open(tmp,FILE_WRITE); if(!out)return false;
  uint8_t zero[PKB_HEADER_SIZE]={0};
  if(out.write(zero,sizeof(zero))!=sizeof(zero)){out.close();SD.remove(tmp);return false;}
  File in=SD.open(src,FILE_READ); if(!in){out.close();SD.remove(tmp);return false;}
  uint16_t recs=0; uint32_t datasz=0, datacrc=0xFFFFFFFFUL, line_no=0;
  while(in.available()) {
    String ls=in.readStringUntil('\n'); ls.trim(); line_no++;
    if(!ls.length()) { if((line_no&31U)==0) vTaskDelay(1); continue; }
    if(ls.length()>=127 || recs==65535U) { in.close();out.close();SD.remove(tmp);return false; }
    char line[128]; ls.toCharArray(line,sizeof(line));
    if(!emit_record(out,line,&recs,&datasz,&datacrc)) {
      console->printf("PKB: unsupported line %lu: %s\n",(unsigned long)line_no,line);
      in.close();out.close();SD.remove(tmp);return false;
    }
    if((recs%250U)==0) console->printf("PKB: streaming %u records\n",(unsigned)recs);
    if((line_no&31U)==0) vTaskDelay(1);
  }
  in.close(); datacrc=~datacrc;
  if(!write_header(out,srcsz,srccrc,recs,datasz,datacrc)){out.close();SD.remove(tmp);return false;}
  out.flush();out.close();
  File verify=SD.open(tmp,FILE_READ); if(!verify)return false; size_t vs=verify.size();verify.close();
  if(vs!=PKB_HEADER_SIZE+datasz){SD.remove(tmp);return false;}
  if(SD.exists(pkb)) SD.remove(pkb);
  if(!SD.rename(tmp,pkb)){SD.remove(tmp);return false;}
  console->printf("PKB: streaming built %s records=%u source=%lu packed=%lu src_crc=%08lX data_crc=%08lX\n",
                  pkb,(unsigned)recs,(unsigned long)srcsz,(unsigned long)(PKB_HEADER_SIZE+datasz),
                  (unsigned long)srccrc,(unsigned long)datacrc);
  console->printf("PKB: streaming timing total=%lu ms; internal heap start=%u end=%u min_before=%u min_after=%u\n",
                  (unsigned long)(millis()-t0),(unsigned)heap_start,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_min_start,(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

static bool build_pkb(const char *src,const char *pkb,const char *tmp,uint32_t srcsz,uint32_t srccrc,uint32_t line_hint) {
#if JK1DVPLOG_HWVER == 1
  (void)line_hint;
  return build_pkb_streaming(src,pkb,tmp,srcsz,srccrc);
#else
  uint32_t t0=millis(), tscan=0, tsort=0, twrite=0;
  size_t heap_start=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t heap_min_start=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

  if(line_hint==0 || line_hint>65535UL) return false;
  PkbSortKey *keys=(PkbSortKey*)heap_caps_malloc((size_t)line_hint*sizeof(PkbSortKey), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!keys) {
    console->printf("PKB: sort-key allocation failed records=%lu key=%u total=%lu internal_free=%u\n",
                    (unsigned long)line_hint,(unsigned)sizeof(PkbSortKey),
                    (unsigned long)((size_t)line_hint*sizeof(PkbSortKey)),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return false;
  }
  console->printf("PKB: sort-key size=%u records<=%lu temp=%lu bytes internal_free=%u\n",
                  (unsigned)sizeof(PkbSortKey),(unsigned long)line_hint,
                  (unsigned long)((size_t)line_hint*sizeof(PkbSortKey)),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  File in=SD.open(src,FILE_READ);
  if(!in){free(keys);return false;}
  uint32_t nkeys=0, line_no=0;
  while(in.available()) {
    uint32_t off=(uint32_t)in.position();
    String ls=in.readStringUntil('\n'); ls.trim(); line_no++;
    if(!ls.length()) { if((line_no&31)==0) vTaskDelay(1); continue; }
    if(ls.length()>=127 || nkeys>=line_hint) { in.close();free(keys);return false; }
    char cand[128]; ls.toCharArray(cand,sizeof(cand));
    char parse[128]; strcpy(parse,cand); char *call,*exch;
    if(!split_line(parse,&call,&exch)) {
      console->printf("PKB: unsupported line: %s\n",cand);
      in.close();free(keys);return false;
    }
    size_t cl=strlen(call);
    if(cl==0 || cl>PKB_SORT_CALL_MAX) {
      console->printf("PKB: callsign too long for sort key (%u>%u): %s\n",
                      (unsigned)cl,(unsigned)PKB_SORT_CALL_MAX,call);
      in.close();free(keys);return false;
    }
    PkbSortKey &k=keys[nkeys++];
    memset(&k,0,sizeof(k)); k.source_offset=off; k.call_len=(uint8_t)cl;
    if(!pack6(call,true,k.call_packed,sizeof(k.call_packed))) {
      in.close();free(keys);return false;
    }
    if((line_no&31)==0) vTaskDelay(1);
  }
  in.close();
  tscan=millis()-t0;

  uint32_t ts=millis();
  qsort(keys,nkeys,sizeof(PkbSortKey),sort_key_cmp);
  tsort=millis()-ts;
  vTaskDelay(1);

  if(SD.exists(tmp)) SD.remove(tmp);
  File out=SD.open(tmp,FILE_WRITE); if(!out){free(keys);return false;}
  uint8_t zero[PKB_HEADER_SIZE]={0};
  if(out.write(zero,sizeof(zero))!=sizeof(zero)){out.close();free(keys);return false;}
  in=SD.open(src,FILE_READ); if(!in){out.close();SD.remove(tmp);free(keys);return false;}

  uint16_t recs=0; uint32_t datasz=0, datacrc=0xFFFFFFFFUL;
  uint32_t tw0=millis();
  for(uint32_t i=0;i<nkeys;i++) {
    if(!in.seek(keys[i].source_offset)) { in.close();out.close();SD.remove(tmp);free(keys);return false; }
    String ls=in.readStringUntil('\n'); ls.trim();
    if(!ls.length() || ls.length()>=127) { in.close();out.close();SD.remove(tmp);free(keys);return false; }
    char line[128]; ls.toCharArray(line,sizeof(line));
    if(!emit_record(out,line,&recs,&datasz,&datacrc)) { in.close();out.close();SD.remove(tmp);free(keys);return false; }
    if(((i+1)%250)==0) console->printf("PKB: packing %lu/%lu records\n",(unsigned long)(i+1),(unsigned long)nkeys);
    if((i&31)==31) vTaskDelay(1);
  }
  in.close();
  twrite=millis()-tw0;
  free(keys); keys=nullptr;
  vTaskDelay(1);

  datacrc=~datacrc;
  if(!write_header(out,srcsz,srccrc,recs,datasz,datacrc)){out.close();SD.remove(tmp);return false;}
  out.flush();out.close();
  File verify=SD.open(tmp,FILE_READ); if(!verify)return false; size_t vs=verify.size();verify.close();
  if(vs!=PKB_HEADER_SIZE+datasz){SD.remove(tmp);return false;}
  if(SD.exists(pkb)) SD.remove(pkb);
  if(!SD.rename(tmp,pkb)){SD.remove(tmp);return false;}

  size_t heap_end=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t heap_min_end=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  console->printf("PKB: built %s records=%u source=%lu packed=%lu src_crc=%08lX data_crc=%08lX\n",
                  pkb,(unsigned)recs,(unsigned long)srcsz,(unsigned long)(PKB_HEADER_SIZE+datasz),
                  (unsigned long)srccrc,(unsigned long)datacrc);
  console->printf("PKB: timing scan=%lu sort=%lu write=%lu total=%lu ms; internal heap start=%u end=%u min_before=%u min_after=%u\n",
                  (unsigned long)tscan,(unsigned long)tsort,(unsigned long)twrite,(unsigned long)(millis()-t0),
                  (unsigned)heap_start,(unsigned)heap_end,(unsigned)heap_min_start,(unsigned)heap_min_end);
  return true;
#endif
}
}

void callhist_source_updated(const char *source_fn) {
  callhist_pkb_release();
  char pkb[20],tmp[20]; if(!make_names(source_fn,pkb,sizeof(pkb),tmp,sizeof(tmp)))return;
  if(SD.exists(pkb)){SD.remove(pkb);console->printf("PKB: invalidated %s after source update\n",pkb);}
  if(SD.exists(tmp))SD.remove(tmp);
}

bool ensure_callhist_pkb(const char *source_fn) {
  char pkb[20],tmp[20]; if(!make_names(source_fn,pkb,sizeof(pkb),tmp,sizeof(tmp)))return false;
  uint32_t sz=0,crc=0,lines=0;if(!fingerprint(source_fn,&sz,&crc,&lines))return false;
  if(header_matches(pkb,sz,crc)){console->printf("PKB: cache valid %s source=%lu bytes crc=%08lX\n",pkb,(unsigned long)sz,(unsigned long)crc);return true;}
  // Do not leave a stale/partial cache in place while rebuilding.  In
  // particular on HW1, a failed rebuild must never be mistaken for a usable
  // SD-resident PKB on the next CALLHIST open.
  if(SD.exists(pkb)) {
    if(SD.remove(pkb)) console->printf("PKB: removed invalid cache %s before rebuild\n",pkb);
    else { console->printf("PKB: failed to remove invalid cache %s\n",pkb); return false; }
  }
  if(SD.exists(tmp)) SD.remove(tmp);
  console->printf("PKB: rebuilding %s from %s (%lu bytes, ~%lu lines)\n",pkb,source_fn,(unsigned long)sz,(unsigned long)lines);
  return build_pkb(source_fn,pkb,tmp,sz,crc,lines);
}

namespace {
static uint8_t *pkb_data = nullptr;
static size_t pkb_data_size = 0;
static int pkb_records = 0;
static bool pkb_in_psram = false;
static uint16_t *pkb_sig = nullptr;

// HW1 no-PSRAM backend: callsigns stay packed in internal RAM while
// exchanges remain in the PKB on SD.  Search is a full RAM scan, so source
// order (sorted or unsorted) is irrelevant.
struct __attribute__((packed)) PkbSdIndex {
  uint16_t offset;       // packed record offset on SD
};
static PkbSdIndex *pkb_sd_index = nullptr;
static uint8_t *pkb_sd_calls = nullptr;
static uint8_t *pkb_sd_call_lens = nullptr; // 5 bits/record, supports LEN_CALLSIGN=16
static size_t pkb_sd_calls_size = 0;
static size_t pkb_sd_lens_size = 0;
static bool pkb_sd_mode = false;
static char pkb_sd_name[20] = {0};
static uint32_t pkb_sd_reads = 0;
static uint32_t pkb_sd_bytes = 0;
static uint32_t pkb_sd_cache_hits = 0;
static uint32_t pkb_sd_cache_misses = 0;
static size_t pkb_sd_read_low_free = (size_t)-1;
static size_t pkb_sd_read_low_largest = (size_t)-1;
static const uint8_t PKB_EXCH_CACHE_N = 4;
struct PkbExchCache { uint16_t rec; uint8_t valid; char exch[LEN_EXCH+1]; };
static PkbExchCache pkb_exch_cache[PKB_EXCH_CACHE_N];
static uint8_t pkb_exch_cache_next = 0;

static void pkb_sd_note_heap() {
  const size_t fr=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  const size_t lg=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(fr<pkb_sd_read_low_free) pkb_sd_read_low_free=fr;
  if(lg<pkb_sd_read_low_largest) pkb_sd_read_low_largest=lg;
}

static bool unpack6(const uint8_t *src,size_t src_bytes,uint8_t nchars,bool call,
                    char *dst,size_t dst_size);
static uint16_t gram_signature(const char *s);

static void pkb_len5_set(uint8_t *p,uint16_t rec,uint8_t v) {
  const uint32_t bit=(uint32_t)rec*5U; const uint32_t byte=bit>>3; const unsigned sh=bit&7U;
  uint16_t w=p[byte]; if(sh>3) w|=(uint16_t)p[byte+1]<<8;
  w=(uint16_t)((w&~((uint16_t)31U<<sh))|((uint16_t)(v&31U)<<sh));
  p[byte]=(uint8_t)w; if(sh>3) p[byte+1]=(uint8_t)(w>>8);
}
static uint8_t pkb_len5_get(const uint8_t *p,uint16_t rec) {
  const uint32_t bit=(uint32_t)rec*5U; const uint32_t byte=bit>>3; const unsigned sh=bit&7U;
  uint16_t w=p[byte]; if(sh>3) w|=(uint16_t)p[byte+1]<<8;
  return (uint8_t)((w>>sh)&31U);
}

static bool pkb_sd_read_record(int rec,uint8_t *buf,size_t cap,size_t *rn) {
  if(!pkb_sd_mode || !pkb_sd_index || rec<0 || rec>=pkb_records || !buf || cap<2) return false;
  pkb_sd_note_heap();
  File f=SD.open(pkb_sd_name,FILE_READ);
  pkb_sd_note_heap();
  if(!f) return false;
  const size_t pos=(size_t)PKB_HEADER_SIZE+(size_t)pkb_sd_index[rec].offset;
  if(!f.seek(pos)) { f.close(); return false; }
  if(f.read(buf,2)!=2) { f.close(); return false; }
  const uint8_t cl=buf[0], el=buf[1];
  const size_t cb=((size_t)cl*6+7)/8, eb=((size_t)el*6+7)/8;
  const size_t n=2+cb+eb;
  if(!cl || !el || n>cap || f.read(buf+2,n-2)!=(int)(n-2)) { f.close(); return false; }
  f.close();
  pkb_sd_note_heap();
  pkb_sd_reads++; pkb_sd_bytes+=(uint32_t)n;
  if(rn) *rn=n;
  return true;
}

static bool pkb_sd_load_index(File &f,const char *pkb,uint16_t recs,uint32_t datasz,uint32_t expect_crc) {
  if(datasz>65535UL || !recs) return false;
  uint32_t off=0; size_t calls_bytes=0; uint8_t hdr[2];
  for(uint16_t i=0;i<recs;i++) {
    if(off+2>datasz || !f.seek((size_t)PKB_HEADER_SIZE+off) || f.read(hdr,2)!=2) return false;
    const uint8_t cl=hdr[0], el=hdr[1];
    const size_t cb=((size_t)cl*6+7)/8, eb=((size_t)el*6+7)/8, rn=2+cb+eb;
    if(!cl || cl>LEN_CALLSIGN || !el || off+rn>datasz) return false;
    calls_bytes+=cb; if(calls_bytes>65535U) return false; off+=(uint32_t)rn;
  }
  if(off!=datasz) return false;
  const size_t index_bytes=(size_t)recs*sizeof(PkbSdIndex);
  const size_t lens_bytes=((size_t)recs*5U+7U)/8U;
  const size_t total_bytes=index_bytes+lens_bytes+calls_bytes;
  console->printf("PKB-SD: compact request records=%u index=%u lens=%u calls=%u total=%u free_internal=%u largest=%u\n",
                  (unsigned)recs,(unsigned)index_bytes,(unsigned)lens_bytes,(unsigned)calls_bytes,(unsigned)total_bytes,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  PkbSdIndex *idx=(PkbSdIndex*)heap_caps_malloc(index_bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  uint8_t *lens=(uint8_t*)heap_caps_calloc(lens_bytes,1,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  uint8_t *calls=(uint8_t*)heap_caps_malloc(calls_bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!idx || !lens || !calls) { if(idx)free(idx); if(lens)free(lens); if(calls)free(calls); return false; }
  uint32_t crc=0xFFFFFFFFUL; off=0; size_t call_off=0; uint8_t recbuf[128];
  for(uint16_t i=0;i<recs;i++) {
    if(off+2>datasz || !f.seek((size_t)PKB_HEADER_SIZE+off) || f.read(recbuf,2)!=2) goto fail;
    const uint8_t cl=recbuf[0], el=recbuf[1];
    const size_t cb=((size_t)cl*6+7)/8, eb=((size_t)el*6+7)/8, rn=2+cb+eb;
    if(!cl || cl>LEN_CALLSIGN || !el || off+rn>datasz || rn>sizeof(recbuf) ||
       call_off+cb>calls_bytes || f.read(recbuf+2,rn-2)!=(int)(rn-2)) goto fail;
    idx[i].offset=(uint16_t)off; pkb_len5_set(lens,i,cl);
    memcpy(calls+call_off,recbuf+2,cb); call_off+=cb;
    crc=crc32_update(crc,recbuf,rn); off+=(uint32_t)rn;
    if((i&127U)==127U) yield();
  }
  crc=~crc;
  if(off!=datasz || call_off!=calls_bytes || crc!=expect_crc) {
    console->printf("PKB-SD: validation failed data=%lu/%lu calls=%u/%u crc=%08lX/%08lX\n",
                    (unsigned long)off,(unsigned long)datasz,(unsigned)call_off,(unsigned)calls_bytes,
                    (unsigned long)crc,(unsigned long)expect_crc); goto fail;
  }
  pkb_sd_index=idx; pkb_sd_call_lens=lens; pkb_sd_calls=calls;
  pkb_sd_lens_size=lens_bytes; pkb_sd_calls_size=calls_bytes; pkb_sd_mode=true;
  strlcpy(pkb_sd_name,pkb,sizeof(pkb_sd_name)); pkb_data_size=datasz; pkb_records=recs; pkb_in_psram=false;
  pkb_sd_reads=0; pkb_sd_bytes=0; pkb_sd_cache_hits=0; pkb_sd_cache_misses=0;
  memset(pkb_exch_cache,0,sizeof(pkb_exch_cache)); pkb_exch_cache_next=0;
  pkb_sd_read_low_free=(size_t)-1; pkb_sd_read_low_largest=(size_t)-1;
  console->printf("PKB-SD: compact ready records=%d RAM=%u bytes (index=%u lens=%u calls=%u) data=%u bytes on SD\n",
                  pkb_records,(unsigned)total_bytes,(unsigned)index_bytes,(unsigned)lens_bytes,
                  (unsigned)calls_bytes,(unsigned)pkb_data_size);
  return true;
fail:
  free(calls); free(lens); free(idx); return false;
}

static bool unpack6(const uint8_t *src,size_t src_bytes,uint8_t nchars,bool call,
                    char *dst,size_t dst_size) {
  if(!dst || dst_size <= nchars) return false;
  uint32_t bits=0; unsigned nbits=0; size_t si=0;
  for(uint8_t i=0;i<nchars;i++) {
    while(nbits<6) {
      if(si>=src_bytes) return false;
      bits=(bits<<8)|src[si++]; nbits+=8;
    }
    nbits-=6;
    int c=(bits>>nbits)&0x3f;
    if(nbits) bits&=((1UL<<nbits)-1); else bits=0;
    char out=0;
    if(c>=1 && c<=10) out=(char)('0'+c-1);
    else if(c>=11 && c<=36) out=(char)('A'+c-11);
    else if(call && c==37) out='/';
    else if(call && c==38) out='-';
    else return false;
    dst[i]=out;
  }
  dst[nchars]='\0';
  return true;
}

// 16-bit two-gram Bloom signature. False positives are harmless because
// candidates are still verified by the normal substring comparison.
static uint16_t gram_signature(const char *s) {
  if(!s || !s[0] || !s[1]) return 0;
  uint16_t sig=0;
  for(size_t i=0;s[i] && s[i+1];i++) {
    const uint8_t a=(uint8_t)toupper((unsigned char)s[i]);
    const uint8_t b=(uint8_t)toupper((unsigned char)s[i+1]);
    const unsigned h1=((unsigned)a*17U+(unsigned)b*31U)&15U;
    const unsigned h2=((unsigned)a*29U+(unsigned)b*13U+7U)&15U;
    sig|=(uint16_t)(1U<<h1);
    sig|=(uint16_t)(1U<<h2);
  }
  return sig;
}

static void pkb_build_signature_index() {
  if(pkb_sig) { free(pkb_sig); pkb_sig=nullptr; }
  if(!pkb_data || pkb_records<=0) return;
  const uint32_t caps=MALLOC_CAP_8BIT |
                      (f_spiram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
  pkb_sig=(uint16_t*)heap_caps_malloc((size_t)pkb_records*sizeof(uint16_t),caps);
  if(!pkb_sig) {
    console->printf("PKB: substring index allocation failed bytes=%u; full scan fallback\n",
                    (unsigned)((size_t)pkb_records*sizeof(uint16_t)));
    return;
  }
  size_t off=0;
  char call[LEN_CALLSIGN+1];
  for(int i=0;i<pkb_records;i++) {
    if(off+2>pkb_data_size) { free(pkb_sig); pkb_sig=nullptr; return; }
    const uint8_t cl=pkb_data[off], el=pkb_data[off+1];
    const size_t cb=((size_t)cl*6+7)/8, eb=((size_t)el*6+7)/8;
    const size_t next=off+2+cb+eb;
    if(!cl || !el || next>pkb_data_size ||
       !unpack6(pkb_data+off+2,cb,cl,true,call,sizeof(call))) {
      free(pkb_sig); pkb_sig=nullptr; return;
    }
    pkb_sig[i]=gram_signature(call);
    off=next;
  }
  console->printf("PKB: substring index ready records=%d memory=%u bytes\n",
                  pkb_records,(unsigned)((size_t)pkb_records*sizeof(uint16_t)));
}

static bool pkb_decode_at(size_t off,char *call,size_t call_size,
                          char *exch,size_t exch_size,size_t *next) {
  if(!pkb_data || off+2>pkb_data_size) return false;
  uint8_t cl=pkb_data[off], el=pkb_data[off+1];
  size_t cb=((size_t)cl*6+7)/8, eb=((size_t)el*6+7)/8;
  size_t end=off+2+cb+eb;
  if(!cl || !el || end>pkb_data_size) return false;
  if(!unpack6(pkb_data+off+2,cb,cl,true,call,call_size)) return false;
  if(!unpack6(pkb_data+off+2+cb,eb,el,false,exch,exch_size)) return false;
  if(next) *next=end;
  return true;
}
}

void callhist_pkb_release() {
  if(pkb_sd_mode)
    console->printf("PKB-SD: release reads=%lu bytes=%lu cache=%lu/%lu sdheap=%u/%u min=%u\n",
                    (unsigned long)pkb_sd_reads,(unsigned long)pkb_sd_bytes,
                    (unsigned long)pkb_sd_cache_hits,(unsigned long)pkb_sd_cache_misses,
                    pkb_sd_read_low_free==(size_t)-1?0:(unsigned)pkb_sd_read_low_free,
                    pkb_sd_read_low_largest==(size_t)-1?0:(unsigned)pkb_sd_read_low_largest,
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  if(pkb_sd_index) free(pkb_sd_index);
  if(pkb_sd_calls) free(pkb_sd_calls);
  if(pkb_sd_call_lens) free(pkb_sd_call_lens);
  pkb_sd_index=nullptr; pkb_sd_calls=nullptr; pkb_sd_call_lens=nullptr;
  pkb_sd_calls_size=0; pkb_sd_lens_size=0;
  pkb_sd_mode=false; pkb_sd_name[0]='\0';
  pkb_sd_reads=0; pkb_sd_bytes=0; pkb_sd_cache_hits=0; pkb_sd_cache_misses=0;
  memset(pkb_exch_cache,0,sizeof(pkb_exch_cache)); pkb_exch_cache_next=0;
  pkb_sd_read_low_free=(size_t)-1; pkb_sd_read_low_largest=(size_t)-1;
  if(pkb_sig) free(pkb_sig);
  pkb_sig=nullptr;
  if(pkb_data) free(pkb_data);
  pkb_data=nullptr; pkb_data_size=0; pkb_records=0; pkb_in_psram=false;
}

bool callhist_pkb_loaded() { return pkb_data != nullptr || pkb_sd_mode; }
size_t callhist_pkb_memory_bytes() {
  if(pkb_sd_mode) return (size_t)pkb_records*sizeof(PkbSdIndex)+pkb_sd_lens_size+pkb_sd_calls_size;
  return pkb_data_size;
}
int callhist_pkb_count() { return pkb_records; }

bool callhist_pkb_load(const char *source_fn) {
  char pkb[20],tmp[20];
  if(!make_names(source_fn,pkb,sizeof(pkb),tmp,sizeof(tmp))) return false;
  File f=SD.open(pkb,FILE_READ); if(!f) return false;
  uint8_t h[PKB_HEADER_SIZE];
  if(f.read(h,sizeof(h))!=(int)sizeof(h) || memcmp(h,PKB_MAGIC,4) ||
     h[4]!=PKB_VERSION || h[5]!=PKB_HEADER_SIZE) { f.close(); return false; }
  uint16_t recs=get16(h+16); uint32_t datasz=get32(h+20); uint32_t expect_crc=get32(h+24);
  if((size_t)f.size()!=PKB_HEADER_SIZE+(size_t)datasz || !recs || !datasz) { f.close(); return false; }

  callhist_pkb_release();
#if JK1DVPLOG_HWVER == 1
  // On HW1 without PSRAM, keep the PKB data on SD and allocate only the
  // 4-byte/record offset + substring-signature index in internal RAM.
  if(!f_spiram) {
    if(pkb_sd_load_index(f,pkb,recs,datasz,expect_crc)) { f.close(); return true; }
    console->println("PKB-SD: index backend unavailable; MAIN load failed");
    f.close();
    return false;
  }
#endif
  uint32_t caps=MALLOC_CAP_8BIT | (f_spiram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
  pkb_data=(uint8_t*)heap_caps_malloc(datasz,caps);
  if(!pkb_data) {
    console->printf("PKB: runtime allocation failed data=%lu RAM=%s free_internal=%u\n",
                    (unsigned long)datasz,f_spiram?"PSRAM":"Internal",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    f.close(); return false;
  }
  int nr=f.read(pkb_data,datasz); f.close();
  if(nr!=(int)datasz) { callhist_pkb_release(); return false; }
  uint32_t crc=~crc32_update(0xFFFFFFFFUL,pkb_data,datasz);
  if(crc!=expect_crc) {
    console->printf("PKB: runtime CRC mismatch got=%08lX want=%08lX\n",
                    (unsigned long)crc,(unsigned long)expect_crc);
    callhist_pkb_release(); return false;
  }
  pkb_data_size=datasz; pkb_records=recs; pkb_in_psram=f_spiram!=0;
  pkb_build_signature_index();
  console->printf("PKB: runtime loaded records=%d data=%u bytes RAM=%s\n",
                  pkb_records,(unsigned)pkb_data_size,pkb_in_psram?"PSRAM":"Internal");
  return true;
}

void callhist_iter_begin(struct callhist_iter *it) {
  if(!it) return;
  it->offset=0;
  it->index=0;
  it->current_offset=(size_t)-1;
  it->filter_sig=0;
  it->filter_enabled=false;
}

void callhist_iter_set_filter(struct callhist_iter *it,const char *needle) {
  if(!it) return;
  it->filter_sig=0;
  it->filter_enabled=false;
  if(pkb_sd_mode || !pkb_sig || !needle || !needle[0] || !needle[1] || strchr(needle,'-')) return;
  const uint16_t sig=gram_signature(needle);
  if(!sig) return;
  it->filter_sig=sig;
  it->filter_enabled=true;
}

bool callhist_iter_next(struct callhist_iter *it,char *call,size_t call_size,
                        char *exch,size_t exch_size) {
  if(!it) return false;
  if(pkb_data) {
    if(it->index>=pkb_records || it->offset>=pkb_data_size) return false;
    size_t next=0;
    if(!pkb_decode_at(it->offset,call,call_size,exch,exch_size,&next)) return false;
    it->offset=next; it->index++; return true;
  }
  if(pkb_sd_mode) {
    if(it->index>=pkb_records) return false;
    uint8_t recbuf[128]; size_t rn=0; const int rec=it->index++;
    if(!pkb_sd_read_record(rec,recbuf,sizeof(recbuf),&rn)) return false;
    const uint8_t cl=recbuf[0], el=recbuf[1];
    const size_t cb=((size_t)cl*6+7)/8, eb=((size_t)el*6+7)/8;
    if(2+cb+eb!=rn || !unpack6(recbuf+2,cb,cl,true,call,call_size) ||
       !unpack6(recbuf+2+cb,eb,el,false,exch,exch_size)) return false;
    it->offset+=cb;
    it->current_offset=(size_t)rec; return true;
  }
  if(!callhist_list || it->index>=n_callhist_list || !callhist_list[it->index] ||
     !callhist_list[it->index][0]) return false;
  const char *src=callhist_list[it->index++];
  const char *sp=strchr(src,' '); if(!sp) return false;
  size_t cl=(size_t)(sp-src); while(*sp==' ') sp++;
  if(cl>=call_size) cl=call_size-1;
  memcpy(call,src,cl); call[cl]='\0'; strlcpy(exch,sp,exch_size);
  return true;
}

// Fast path for callsign matching.  Do not unpack the exchange until the
// caller knows that this record is actually a candidate.
bool callhist_iter_next_call(struct callhist_iter *it,char *call,size_t call_size) {
  if(!it || !call || !call_size) return false;
  if(pkb_data) {
    while(it->index<pkb_records && it->offset<pkb_data_size) {
      const size_t off=it->offset;
      const int rec=it->index;
      if(off+2>pkb_data_size) return false;
      const uint8_t cl=pkb_data[off], el=pkb_data[off+1];
      const size_t cb=((size_t)cl*6+7)/8;
      const size_t eb=((size_t)el*6+7)/8;
      const size_t next=off+2+cb+eb;
      if(!cl || !el || next>pkb_data_size) return false;
      it->offset=next;
      it->index++;
      if(it->filter_enabled && pkb_sig &&
         (pkb_sig[rec]&it->filter_sig)!=it->filter_sig) continue;
      if(!unpack6(pkb_data+off+2,cb,cl,true,call,call_size)) return false;
      it->current_offset=off;
      return true;
    }
    return false;
  }
  if(pkb_sd_mode) {
    while(it->index<pkb_records) {
      const int rec=it->index++;
      const uint8_t cl=pkb_len5_get(pkb_sd_call_lens,(uint16_t)rec);
      const size_t cb=((size_t)cl*6+7)/8, co=it->offset;
      if(!cl || cl>LEN_CALLSIGN || !pkb_sd_calls || co+cb>pkb_sd_calls_size ||
         !unpack6(pkb_sd_calls+co,cb,cl,true,call,call_size)) return false;
      it->offset=co+cb;
      it->current_offset=(size_t)rec;
      return true;
    }
    return false;
  }
  if(!callhist_list || it->index>=n_callhist_list || !callhist_list[it->index] ||
     !callhist_list[it->index][0]) return false;
  const char *src=callhist_list[it->index];
  const char *sp=strchr(src,' ');
  if(!sp) return false;
  size_t cl=(size_t)(sp-src);
  if(cl>=call_size) cl=call_size-1;
  memcpy(call,src,cl);
  call[cl]='\0';
  it->current_offset=(size_t)it->index;
  it->index++;
  return true;
}

bool callhist_iter_current_exch(struct callhist_iter *it,char *exch,size_t exch_size) {
  if(!it || !exch || !exch_size || it->current_offset==(size_t)-1) return false;
  if(pkb_data) {
    const size_t off=it->current_offset;
    if(off+2>pkb_data_size) return false;
    const uint8_t cl=pkb_data[off], el=pkb_data[off+1];
    const size_t cb=((size_t)cl*6+7)/8;
    const size_t eb=((size_t)el*6+7)/8;
    if(!cl || !el || off+2+cb+eb>pkb_data_size) return false;
    return unpack6(pkb_data+off+2+cb,eb,el,false,exch,exch_size);
  }
  if(pkb_sd_mode) {
    const int rec=(int)it->current_offset;
    for(uint8_t i=0;i<PKB_EXCH_CACHE_N;i++) {
      if(pkb_exch_cache[i].valid && pkb_exch_cache[i].rec==(uint16_t)rec) {
        strlcpy(exch,pkb_exch_cache[i].exch,exch_size);
        pkb_sd_cache_hits++;
        return true;
      }
    }
    pkb_sd_cache_misses++;
    uint8_t recbuf[128]; size_t rn=0;
    if(!pkb_sd_read_record(rec,recbuf,sizeof(recbuf),&rn)) return false;
    const uint8_t cl=recbuf[0], el=recbuf[1];
    const size_t cb=((size_t)cl*6+7)/8, eb=((size_t)el*6+7)/8;
    char tmp[LEN_EXCH+1];
    if(!unpack6(recbuf+2+cb,eb,el,false,tmp,sizeof(tmp))) return false;
    PkbExchCache &ce=pkb_exch_cache[pkb_exch_cache_next];
    ce.rec=(uint16_t)rec; ce.valid=1; strlcpy(ce.exch,tmp,sizeof(ce.exch));
    pkb_exch_cache_next=(uint8_t)((pkb_exch_cache_next+1U)%PKB_EXCH_CACHE_N);
    strlcpy(exch,tmp,exch_size);
    return true;
  }
  const int idx=(int)it->current_offset;
  if(!callhist_list || idx<0 || idx>=n_callhist_list || !callhist_list[idx]) return false;
  const char *sp=strchr(callhist_list[idx],' ');
  if(!sp) return false;
  while(*sp==' ') sp++;
  strlcpy(exch,sp,exch_size);
  return true;
}

bool callhist_lookup_exact(const char *callsign,char *exch,size_t exch_size) {
  if(!callsign || !exch || !exch_size) return false;
  struct callhist_iter it; callhist_iter_begin(&it);
  callhist_iter_set_filter(&it,callsign);
  char c[LEN_CALLSIGN+1];
  while(callhist_iter_next_call(&it,c,sizeof(c))) {
    if(dupe_callsign_equal(c,callsign))
      return callhist_iter_current_exch(&it,exch,exch_size);
  }
  return false;
}

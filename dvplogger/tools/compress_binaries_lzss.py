#!/usr/bin/env python3
import argparse, re
from collections import defaultdict, deque

WINDOW=2048
MAXLEN=34
MAX_CHAIN=64
NAMES=('bootloader_bootloader_bin','partition_table_partition_table_bin','jk1dvplog_ext_bin')

def parse_array(text,name):
    m=re.search(r'const uint8_t\s+'+re.escape(name)+r'\[\]\s*=\s*\{(.*?)\};',text,re.S)
    if not m: raise SystemExit('array not found: '+name)
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',m.group(1)))

def parse_md5(text,name):
    m=re.search(r'const uint8_t\s+'+re.escape(name)+r'_md5\[\]\s*=\s*"([0-9a-fA-F]+)"',text)
    return m.group(1) if m else ''

def compress(data):
    out=bytearray(); table=defaultdict(deque); i=0
    while i < len(data):
        cp=len(out); out.append(0); ctrl=0
        for bit in range(8):
            if i >= len(data): break
            best_len=best_off=0
            if i+3 <= len(data):
                key=data[i:i+3]; q=table[key]
                while q and i-q[0] > WINDOW: q.popleft()
                for pos in reversed(list(q)[-MAX_CHAIN:]):
                    limit=min(MAXLEN,len(data)-i); ln=3
                    while ln < limit and data[pos+ln] == data[i+ln]: ln += 1
                    if ln > best_len:
                        best_len,best_off=ln,i-pos
                        if ln == limit: break
            if best_len >= 3:
                ctrl |= 1 << bit
                v=((best_off-1)<<5)|(best_len-3)
                out.extend((v & 0xff, (v>>8)&0xff))
                end=i+best_len
                while i < end:
                    if i+3 <= len(data):
                        q=table[data[i:i+3]]; q.append(i)
                        if len(q)>128: q.popleft()
                    i += 1
            else:
                out.append(data[i])
                if i+3 <= len(data):
                    q=table[data[i:i+3]]; q.append(i)
                    if len(q)>128: q.popleft()
                i += 1
        out[cp]=ctrl
    return bytes(out)

def emit_array(f,name,data):
    f.write('const uint8_t %s_lzss[] = {\n' % name)
    for i in range(0,len(data),16):
        f.write('  '+','.join('0x%02x'%x for x in data[i:i+16])+',\n')
    f.write('};\n')
    f.write('const uint32_t %s_lzss_size = sizeof(%s_lzss);\n' % (name,name))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--input',required=True); ap.add_argument('--output',required=True); a=ap.parse_args()
    text=open(a.input,encoding='ascii').read()
    with open(a.output,'w',encoding='ascii') as f:
        f.write('#include <stdint.h>\n/* Auto-generated from binaries.c: LZSS W=2048, max match=34. */\n')
        for name in NAMES:
            raw=parse_array(text,name); enc=compress(raw); emit_array(f,name,enc)
            f.write('const uint32_t %s_size = %dU;\n' % (name,len(raw)))
            f.write('const uint8_t %s_md5[] = "%s";\n' % (name,parse_md5(text,name)))
            print('%s: %d -> %d (saved %d)'%(name,len(raw),len(enc),len(raw)-len(enc)))
if __name__=='__main__': main()

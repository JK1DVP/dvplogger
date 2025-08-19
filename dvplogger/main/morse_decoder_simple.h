#ifndef FILE_MORSE_DECODER_SIMPLE_H
#define FILE_MORSE_DECODER_SIMPLE_H
#include "Arduino.h"
#include "decl.h"
#include "RingBuffer.h"  

///////////////////////////////////////////////////////////////////////
// CW Decoder made by Hjalmar Skovholm Hansen OZ1JHM  VER 1.01       //
// Feel free to change, copy or what ever you like but respect       //
// that license is http://www.gnu.org/copyleft/gpl.html              //
// Discuss and give great ideas on                                   //
// https://groups.yahoo.com/neo/groups/oz1jhm/conversations/messages //
///////////////////////////////////////////////////////////////////////

class Morse_decoder {
public:
  Morse_decoder()  {
    setup();
  };
      
private:

  float magnitude ;
  int magnitudelimit = 100;
  int magnitudelimit_low = 100;
  int realstate = 0;
  int realstatebefore = 0;
  int filteredstate = 0;
  int filteredstatebefore = 0;


  ///////////////////////////////////////////////////////////
  // The sampling frq will be 8928 on a 16 mhz             //
  // without any prescaler etc                             //
  // because we need the tone in the center of the bins    //
  // you can set the tone to 496, 558, 744 or 992          //
  // then n the number of samples which give the bandwidth //
  // can be (8928 / tone) * 1 or 2 or 3 or 4 etc           //
  // init is 8928/558 = 16 *4 = 64 samples                 //
  // try to take n = 96 or 128 ;o)                         //
  // 48 will give you a bandwidth around 186 hz            //
  // 64 will give you a bandwidth around 140 hz            //
  // 96 will give you a bandwidth around 94 hz             //
  // 128 will give you a bandwidth around 70 hz            //
  // BUT remember that high n take a lot of time           //
  // so you have to find the compromice - i use 48         //
  ///////////////////////////////////////////////////////////

  float coeff;
  float Q1 = 0;
  float Q2 = 0;
  float sine;
  float cosine;  

  float target_freq=666.0; /// adjust for your needs see above
#define SAMPLING_FREQ 8000
#define GOERTZEL_DECIM 3  // 24000 -> decim by GOERTZEL_DECIM
#define GOERTZEL_N 48
  float sampling_freq=SAMPLING_FREQ;

  //////////////////////////////
  // Noise Blanker time which //
  // shall be computed so     //
  // this is initial          //
  //////////////////////////////
  int nbtime = 6;  /// ms noise blanker
  float goertzel_unit_ms;
  long goertzel_counter=0;
  long starttimehigh;
  long highduration;
  long lasthighduration;
  long hightimesavg;
  long lowtimesavg;
  long startttimelow;
  long lowduration;
  long laststarttime = 0;
  int  adcmean=913;

  char code[20];
  char decoded[50];
  int f_disp_update;
  int stop = 0;
  int wpm;


  void setup();
  void shift_string(char *s,int size,int n); // size: maximum number of char copied
  void put_code(char *s);  
  void printascii(int c);
  void docode();

  float     goertzel_in[GOERTZEL_N];
  uint16_t  goertzel_fill = 0;

  // 12bit上位詰め/下位詰めの自動判定用 // not used
  static bool     s_adc_upper12;   // true: 上位詰め（>>4）
  static uint8_t  s_adc_seen;      // 判定サンプル数カウンタ
  
  // magnitude を渡して状態機械を回す
  void process_magnitude(float mag);

  // setting I2S0 and ADC
#define I2S_PORT        I2S_NUM_0
#define ADC_UNIT_USE    ADC_UNIT_1
#define ADC_CH          ADC1_CHANNEL_0          
#define ADC_ATTEN_SET   ADC_ATTEN_DB_11
#define FS_TARGET_HZ    (SAMPLING_FREQ*GOERTZEL_DECIM)   // 24 kHz
#define DMA_BUF_LEN     (GOERTZEL_N*GOERTZEL_DECIM)      
#define DMA_BUF_COUNT   8
#define TASK_STACK      4096
#define TASK_PRIO       1
#define TASK_CORE       1                       // Core1 に固定
  struct RmsBlock {
    float    mag; 
  }  ;

  static RingBuffer<RmsBlock> g_rms_rb;
  static SemaphoreHandle_t    g_rb_mtx ;
  static TaskHandle_t         s_i2s_task ;
  static volatile bool        s_run ;
  static volatile uint32_t    s_drop ;
  static volatile float fs_eff;

  static uint16_t adc12_from_i2s(uint16_t s) ;
  static bool rb_push(const RmsBlock &blk) ;
  bool pop_rms_block(RmsBlock &out);
  static void i2s_adc_rms_task(void *arg);
  static bool is_utf8_lead_byte(unsigned char c);
  static void truncate_tail_utf8(char *str, size_t keep_chars);

  static TaskHandle_t s_mon_task;   // 監視タスク
  static void i2s_adc_rms_task_adcinit();
  static void i2s_adc_task_i2sread();

  static int64_t timeout_ticker1,timeout_ticker2,timeout_ticker3,timeout_ticker4;
public:
  static void monitor_task(); // print decoded result
  static void morse_decode_task(); // called in main loop  

  // I2Sブロック（16bit × n）をGoertzelに供給
  void feed_i2s_block_for_goertzel(const uint16_t* buf16, uint16_t n);
  
  void init_morse_decoder()  ;
  void start_i2s_adc_24k_rms_task();
  void stop_i2s_adc_24k_rms_task();

};
extern Morse_decoder decoder;

#endif


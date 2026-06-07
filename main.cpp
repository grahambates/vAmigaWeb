/*!
 * @file        main.cpp
 * @author      mithrendal and Dirk W. Hoffmann, www.dirkwhoffmann.de
 * @copyright   Dirk W. Hoffmann. All rights reserved.
 */

#include <stdio.h>
#include <unordered_map>
#include <unordered_set>
#include "config.h"
#include "VAmiga.h"
#include "VAmigaTypes.h"
#include "Emulator.h"
#include "Amiga.h"
#include "AmigaTypes.h"
#include "RomFile.h"
#include "ADFFile.h"
#include "DMSFile.h"
#include "EXEFile.h"
#include "HDFFile.h"
#include "Snapshot.h"
#include "EADFFile.h"
#include "STFile.h"
#include "OtherFile.h"
#include "MutableFileSystem.h"
#include "OSDebugger.h"
#include "CpuProfiler.h" // [vscode-vamiga-debugger cpu profiler]
#include "DmaProfiler.h" // [vscode-vamiga-debugger dma profiler]

#include "MemUtils.h"
#include "MediaFileTypes.h"

#include <emscripten.h>

#ifdef wasm_worker
#include <emscripten/wasm_worker.h>
#include <emscripten/threading.h>
#endif

using namespace vamiga;

#define RENDER_SOFTWARE 0
#define RENDER_GPU 1
#define RENDER_SHADER 2
u8 render_method = RENDER_SOFTWARE;

#define DISPLAY_NARROW   0
#define DISPLAY_STANDARD 1
#define DISPLAY_WIDER    2
#define DISPLAY_OVERSCAN 3
#define DISPLAY_ADAPTIVE 4
#define DISPLAY_BORDERLESS 5
#define DISPLAY_EXTREME 6
u8 geometry  = DISPLAY_ADAPTIVE;


const char* display_names[] = {"narrow","standard","wider","overscan",
"viewport tracking","borderless","extreme"}; 

bool log_on=false;


//HRM: In NTSC, the fields are 262, then 263 lines and in PAL, 312, then 313 lines.
//312-50=262
#define PAL_EXTRA_VPIXEL 40 //50

#define PAL_FPS 50.0 //50.080128
#define NTSC_FPS 60.0 //59.94
bool ntsc=false;
double target_fps = PAL_FPS;

extern "C" void wasm_set_display(const char *name);
#ifdef wasm_worker
EM_JS(void, console_log, (const char* str), {
  console.log(UTF8ToString(str));
});
void log_on_main_thread(const char *msg)
{
  console_log(msg);
  //printf("%s",msg);
  //assert(!emscripten_current_thread_is_wasm_worker());
}
void main_log(const char *msg)
{
  //call function on ui thread
  emscripten_wasm_worker_post_function_sig(EMSCRIPTEN_WASM_WORKER_ID_PARENT,(void *) log_on_main_thread, "id", msg);
}
#endif

int clip_width  = 724 * TPP;
int clip_height = 568;
int clip_offset = 0;//HBLANK_MIN*4;//HBLANK_MAX*4; 
int buffer_size = 4096;

bool prevLOF = false;
bool currLOF = false;

int eat_border_width = 0;
int eat_border_height = 0;
int xOff = 12 + eat_border_width;
int yOff = 12 + eat_border_height;
int clipped_width  = HPIXELS*TPP -12 -24 -2*eat_border_width; //392
int clipped_height = VPOS_MAX -12 -24 -2*eat_border_height; //248

int bFullscreen = false;

char *filename = NULL;


bool requested_targetFrameCount_reset=false; 

//unsigned int warp_to_frame=0;
int sum_samples=0;
double last_time = 0.0 ;
double last_time_calibrated = 0.0 ;
unsigned int executed_frame_count=0;
int64_t total_executed_frame_count=0;
double start_time=0;//emscripten_get_now();
unsigned int rendered_frame_count=0;
unsigned int frames=0, seconds=0;

double speed_boost=1.0;
bool vsync = false;
signed vsync_speed=2;
u8 vframes=0;
unsigned long current_frame=100;
unsigned host_refresh_rate=60, last_host_refresh_rate=60;
unsigned host_refresh_count=0;
signed boost_param=100;
void calibrate_boost(signed boost_param);

u16 vstart_min=PAL::VBLANK_CNT;
u16 vstop_max=256;
u16 hstart_min=200;
u16 hstop_max=HPIXELS;

u16 vstart_min_tracking=NTSC::VBLANK_CNT;
u16 vstop_max_tracking=256;
u16 hstart_min_tracking=200;
u16 hstop_max_tracking=HPIXELS;
bool reset_calibration=true;
bool request_to_reset_calibration=false;

void set_viewport_dimensions()
{
//    hstart_min=0; hstop_max=HPIXELS;
    
    auto _vstop_max = vstop_max;
    if(ntsc)
    {
      if(vstop_max > NTSC::VPOS_MAX)
        _vstop_max = NTSC::VPOS_MAX;
    }
    else
    {
     if(vstop_max > PAL::VPOS_MAX) 
        _vstop_max = PAL::VPOS_MAX; //312
    }
 
    if(log_on) printf("calib: set_viewport_dimensions hmin=%d, hmax=%d, vmin=%d, vmax=%d vmax_clipped=%d\n",hstart_min,hstop_max,vstart_min, vstop_max, _vstop_max);

    if(render_method==RENDER_SHADER)
    {
      if(geometry == DISPLAY_ADAPTIVE || geometry == DISPLAY_BORDERLESS)
      {         
        xOff = hstart_min;
        yOff = vstart_min;
        clipped_width = hstop_max-hstart_min;
        clipped_height = _vstop_max-vstart_min;
      } 
    }
    else
    {  
      if(geometry== DISPLAY_ADAPTIVE || geometry == DISPLAY_BORDERLESS)
      {         
        xOff = hstart_min;
        yOff = vstart_min;
        clipped_width = hstop_max-hstart_min;
        clipped_height = _vstop_max-vstart_min;
      }
    }
    if(clipped_height>0 && clipped_width>0)
    {
      EM_ASM({js_set_display($0,$1,$2,$3); scaleVMCanvas();},xOff, yOff, clipped_width*TPP,clipped_height );
    }
}



u16 vstart_min_calib=0;
u16 vstop_max_calib=0;
u16 hstart_min_calib=0;
u16 hstop_max_calib=0;
bool calculate_viewport_dimensions(u32 *texture)
{
  if(reset_calibration)
  {
      //printf("reset_calibration %d %d\n",vstop_max_tracking, vstart_min_tracking);
      //first call after new viewport size
      //set start values to the opposite max. borderpos (scan area)  
      vstart_min_calib = vstop_max_tracking; 
      vstop_max_calib = vstart_min_tracking;
      hstart_min_calib = hstop_max_tracking;
      hstop_max_calib = hstart_min_tracking;  
      reset_calibration=false;

  }

  bool pixels_found=false;
  //top border: get vstart_min from texture
  u32 ref_pixel= texture[(HPIXELS*vstart_min_tracking + hstart_min_tracking)*TPP];
//    printf("refpixel:%u\n",ref_pixel);
  for(int y=vstart_min_tracking;y<vstart_min_calib && !pixels_found;y++)
  {
//      printf("\nvstart_line:%u\n",y);
    for(int x=hstart_min_tracking;x<hstop_max_tracking;x++){
      u32 pixel= texture[(HPIXELS*y + x)*TPP];
//        printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
//        printf("\nfirst_pos=%d, vstart_min=%d, vstart_min_track=%d\n",y, vstart_min, vstart_min_tracking);
        vstart_min_calib= y<vstart_min_calib?y:vstart_min_calib;
        break;
      }
    }
  }
  
  //bottom border: get vstop_max from texture
  pixels_found=false;
  ref_pixel= texture[ (HPIXELS*vstop_max_tracking + hstart_min_tracking)*TPP];
//    printf("refpixel:%u\n",ref_pixel);
//    printf("hstart:%u,hstop:%u\n",hstart_min,hstop_max);
  
  for(int y=vstop_max_tracking;y>vstop_max_calib && !pixels_found;y--)
  {
//      printf("\nline:%u\n",y);
    for(int x=hstart_min_tracking;x<hstop_max_tracking;x++){
      u32 pixel= texture[(HPIXELS*y + x)*TPP];
//        printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
        y++; //this line has pixels, so put vstop_max to the next line
//        printf("\ncalib: last_pos=%d, vstop_max=%d, vstop_max_tracking%d\n",y, vstop_max, vstop_max_tracking);
        vstop_max_calib= y>vstop_max_calib?y:vstop_max_calib;
        break;
      }
    }
  }

  //left border: get hstart_min from texture
  pixels_found=false;
  ref_pixel= texture[ (HPIXELS*vstart_min_tracking + hstart_min_tracking-1)*TPP];

  for(int x=hstart_min_tracking-1;x<hstart_min_calib;x++)
  {
//      printf("\nrow:%u\n",x);
    for(int y=vstart_min_calib;y<vstop_max_calib && !pixels_found;y++)
    {
      u32 pixel= texture[(HPIXELS*y + x)*TPP];
//        printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
//          printf("\nlast_xpos=%d, hstop_max=%d\n",x, hstop_max);
        hstart_min_calib=x<hstart_min_calib?x:hstart_min_calib;
        break;
      }
    }
  }

  //right border: get hstop_max from texture
  pixels_found=false;
  ref_pixel= texture[ (HPIXELS*vstart_min_tracking + hstop_max_tracking)*TPP];
//  printf("hstop_max_tracking %d\n",hstop_max_tracking);
//  printf("hstop_max_calib %d\n",hstop_max_calib);
//  printf("ref_pixel %d\n",ref_pixel);

  for(int x=hstop_max_tracking;x>hstop_max_calib;x--)
  {
//     printf("\ncol:%u\n",x);
    for(int y=vstart_min_calib;y<vstop_max_calib && !pixels_found;y++)
    {
      u32 pixel= texture[(HPIXELS*y + x)*TPP];
//      printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
        x++; //this line has pixels, so put hstop_max to the next line
//          printf("\nlast_xpos=%d, hstop_max=%d\n",x, hstop_max);
        hstop_max_calib= x>hstop_max_calib?x:hstop_max_calib;
        break;
      }
    }
  }
//  printf("->hstop_max_calib %d\n",hstop_max_calib);

  bool dimensions_changed=false;

  //handdisk start pixel 416, stop pixel 676
  #define HANDSTART_PIXEL 416
  #define HANDSTOP_PIXEL 676


  if(hstart_min_calib > HANDSTART_PIXEL-32)
    hstart_min_calib= HANDSTART_PIXEL-32;
  if(hstop_max_calib < HANDSTOP_PIXEL+32)
    hstop_max_calib= HANDSTOP_PIXEL+32;

  if(hstart_min_calib < hstop_max_calib )
  {
    if(hstart_min!=hstart_min_calib)
    {
      hstart_min=hstart_min_calib;
      dimensions_changed=true;
    }
    if(hstop_max != hstop_max_calib)
    {
      hstop_max = hstop_max_calib;
      dimensions_changed=true;
    }
  }

  if(vstart_min_calib > VPOS_MAX/4)
    vstart_min_calib= VPOS_MAX/4;
  if(vstop_max_calib < 3*VPOS_MAX/4)
    vstop_max_calib= 3*VPOS_MAX/4;

  if(vstart_min_calib< vstop_max_calib)
  {
    if(vstart_min != vstart_min_calib)
    {
      vstart_min=vstart_min_calib;
      dimensions_changed=true;
    }
    if(vstop_max != vstop_max_calib)
    {
      vstop_max=vstop_max_calib;   
      dimensions_changed=true;
    }
 
  }
//  printf("\nCALIBRATED: (%d,%d) (%d,%d) \n",hstart_min, vstart_min, hstop_max, vstop_max);

  //printf("calib dimensions changed=%d\n",dimensions_changed);
  return dimensions_changed;
}


#define MAX_GAP 5
VAmiga *emu=NULL;
u8 executed_since_last_host_frame=0;
extern "C" void wasm_execute()
{
  emu->emu->computeFrame(); //execute();
  executed_since_last_host_frame++;
  executed_frame_count++;
  total_executed_frame_count++;
}
#ifdef wasm_worker
char wasm_log_buffer[256];
#endif
extern "C" int wasm_draw_one_frame(double now)
//int draw_one_frame_into_SDL(void *thisAmiga, float now) 
{

  //this method is triggered by
  //emscripten_set_main_loop_arg(em_arg_callback_func func, void *arg, int fps, int simulate_infinite_loop) 
  //which is called inside te c64.cpp
  //fps Setting int <=0 (recommended) uses the browser’s requestAnimationFrame mechanism to call the function.

  //The number of callbacks is usually 60 times per second, but will 
  //generally match the display refresh rate in most web browsers as 
  //per W3C recommendation. requestAnimationFrame() 
  
  //double now = emscripten_get_now();  

  double elapsedTimeInSeconds = (now - start_time)/1000.0;
  int64_t targetFrameCount = (int64_t)(elapsedTimeInSeconds * target_fps *speed_boost);
  
  emu->emu->update();

  bool show_stat=true;
  if(emu->isWarping() == true)
  {
    if(log_on) printf("warping at least 25 frames at once ...\n");
    int i=25;
    while(emu->isWarping() == true && i>0)
    {
      emu->emu->computeFrame();
      i--;
    }
    start_time=now;
    total_executed_frame_count=0;
    targetFrameCount=1;
    show_stat=false;
  }

  if(requested_targetFrameCount_reset)
  {
    start_time=now;
    total_executed_frame_count=0;
    targetFrameCount=1;
    requested_targetFrameCount_reset=false;
  }

  if(show_stat)
  {
    //lost the sync
    if(targetFrameCount-total_executed_frame_count > MAX_GAP)
    {
        if(log_on) {
#ifdef wasm_worker          
          snprintf(wasm_log_buffer,sizeof(wasm_log_buffer),"lost sync target=%lld, total_executed=%lld\n", targetFrameCount, total_executed_frame_count);
          main_log(wasm_log_buffer);
#else
          printf("lost sync target=%lld, total_executed=%lld\n", targetFrameCount, total_executed_frame_count);
#endif
        }
        //reset timer
        //because we are out of sync, we do now skip max_gap-1 emulation frames 
        start_time=now;
        total_executed_frame_count=0;
        targetFrameCount=1;  //we are hoplessly behind but do at least one in this round  
    }
    
    host_refresh_count++;
    if(now-last_time>= 1000.0)
    { 
      double passed_time= now - last_time;
      last_time = now;

      seconds += 1; 
      frames += rendered_frame_count;
      if(log_on) {
#ifdef wasm_worker
       snprintf(wasm_log_buffer,sizeof(wasm_log_buffer),"time[ms]=%.0lf, audio_samples=%d, frames [executed=%u, rendered=%u] avg_fps=%u\n", 
       passed_time, sum_samples, executed_frame_count, rendered_frame_count, frames/seconds);
       main_log(wasm_log_buffer);
#else        
      printf("time[ms]=%.0lf, audio_samples=%d, frames [executed=%u, rendered=%u] avg_fps=%u\n", 
      passed_time, sum_samples, executed_frame_count, rendered_frame_count, frames/seconds);
#endif
      }
      host_refresh_rate=host_refresh_count;
      host_refresh_count=0;
      sum_samples=0; 
      rendered_frame_count=0;
      executed_frame_count=0;
    }
  }

  int behind=0;
  if(vsync)
  {
    //current_frame=0, vsync_speed=-2, vframes=0
    //printf("current_frame=%ld, vsync_speed=%d, vframes=%d\n", current_frame, vsync_speed, vframes); 
    current_frame++;
    
    if(vsync_speed<0)
    {    
      if(current_frame % (vsync_speed*-1) !=0)
      {
//        printf("skip frame %ld\n", current_frame % ((unsigned long)vsync_speed*-1) );
        return -1;
      }
      else{
        emu->emu->computeFrame();
//        printf("compute_frame \n"); 
        executed_frame_count++;
        total_executed_frame_count++;
      }
    }
    else
    {
      //0 + 0 < 0 -2
      while(current_frame+vframes  < current_frame + vsync_speed)
      {
        emu->emu->computeFrame();
        //printf("compute_frame \n"); 
        executed_frame_count++;
        total_executed_frame_count++;
        vframes++;
      }
      //printf("\n"); 
      vframes=0;
    }
    //check current frame rate +-1 in case user changed it on host system
    if(abs((long) (host_refresh_rate-last_host_refresh_rate))>1)
    {
      calibrate_boost(boost_param);
      last_host_refresh_rate=host_refresh_rate;
    }
  }
  else
  {
    behind=targetFrameCount-total_executed_frame_count;
    if(behind<=0 && executed_since_last_host_frame==0)
      return -1;   //don't render if ahead of time and everything is already drawn
  }
#ifndef wasm_worker    
  if(behind>0)
  {
    emu->emu->computeFrame();

    executed_frame_count++;
    total_executed_frame_count++;
    behind--;
  }
#endif
  executed_since_last_host_frame=0;
  rendered_frame_count++;

  if(geometry == DISPLAY_BORDERLESS)
  {
//    printf("calibration count=%f\n",now-last_time_calibrated);
    if(now-last_time_calibrated >= 700.0)
    {  
      last_time_calibrated=now;
      auto stable_ptr = emu->videoPort.getTexture(); 

      bool dimensions_changed=calculate_viewport_dimensions((u32 *)stable_ptr - HBLANK_MIN*4*TPP);
      if(dimensions_changed)
      {
#ifdef wasm_worker
        emscripten_wasm_worker_post_function_v(EMSCRIPTEN_WASM_WORKER_ID_PARENT,set_viewport_dimensions);
#else
        set_viewport_dimensions();
#endif
      }  

      if(request_to_reset_calibration)
      {
        reset_calibration=true;
        request_to_reset_calibration=false;
      }
    }
  }
  return behind;
}


int sample_size=0;


void send_message_to_js(const char * msg)
{
    EM_ASM(
    {
        if (typeof message_handler === 'undefined')
            return;
        message_handler( "MSG_"+UTF8ToString($0) );
    }, msg );    

}


void send_message_to_js_main_thread(const char * msg, long data1, long data2)
{
    EM_ASM(
    {
        if (typeof message_handler === 'undefined')
            return;
        message_handler( "MSG_"+UTF8ToString($0), $1, $2 );
    }, msg, data1, data2 );    

}


void send_message_to_js_with_param(const char * message_as_string, long data1, long data2)
{

    if(log_on)
    {
#ifdef wasm_worker          
      if(emscripten_current_thread_is_wasm_worker())
      {
        snprintf(wasm_log_buffer,sizeof(wasm_log_buffer),"worker: vAmiga message=%s, data1=%ld, data2=%ld\n", message_as_string, data1, data2);
        main_log(wasm_log_buffer);
      }
      else
      {
        printf("main: vAmiga message=%s\n", message_as_string);
      }
#else
      printf("vAmiga message=%s, data1=%ld, data2=%ld\n", message_as_string, data1, data2);
#endif
    }


#ifdef wasm_worker
    if(emscripten_current_thread_is_wasm_worker())
    {
      emscripten_wasm_worker_post_function_sig(EMSCRIPTEN_WASM_WORKER_ID_PARENT,(void*)send_message_to_js_main_thread, "iii", message_as_string, data1, data2);
    }
    else
      send_message_to_js_main_thread(message_as_string, data1, data2);
#else
    send_message_to_js_main_thread(message_as_string, data1, data2);
#endif
}

/*
void send_message_to_js_w(const char * msg, long data1, long data2)
{
    send_message_to_js_main_thread(msg,data1,data2);
}
*/


//bool paused_the_emscripten_main_loop=false;
bool already_run_the_emscripten_main_loop=false;
bool warp_mode=false;

// Store the last message for retrieval
static Message lastMessage;
static bool hasLastMessage = false;
//void theListener(const void * amiga, long type,  int data1, int data2, int data3, int data4){
void theListener(const void * emu, Message msg){
  // Store the message for wasm_get_current_message()
  lastMessage = msg;
  hasLastMessage = true;

  int data1=msg.value;
  int data2=0;
  if(msg.type == Msg::VIEWPORT)
  {
    if(msg.viewport.hstrt==0 && msg.viewport.vstrt==0 && msg.viewport.hstop == 0 && msg.viewport.vstop == 0)
      return;
    hstart_min= msg.viewport.hstrt; 
    vstart_min= msg.viewport.vstrt;
    hstop_max=  msg.viewport.hstop;
    vstop_max=  msg.viewport.vstop;
    if(log_on) printf("tracking MSG_VIEWPORT=%d, %d, %d, %d\n",hstart_min, vstart_min, hstop_max, vstop_max);

    hstart_min *=2;
    hstop_max *=2;

    hstart_min=hstart_min<(208-48) ? (208-48):hstart_min;
    hstop_max=hstop_max>(HPIXELS+HBLANK_MAX)? (HPIXELS+HBLANK_MAX):hstop_max;

    if(vstart_min < (ntsc ? NTSC::VBLANK_CNT : PAL::VBLANK_CNT)) 
      vstart_min = (ntsc ? NTSC::VBLANK_CNT : PAL::VBLANK_CNT);

    
    if(log_on)
    {
#ifdef wasm_worker          
      snprintf(wasm_log_buffer,sizeof(wasm_log_buffer),"tracking MSG_VIEWPORT=%u %u %u %u\n",hstart_min, vstart_min, hstop_max, vstop_max);
      main_log(wasm_log_buffer);
#else
      printf("tracking MSG_VIEWPORT=%u %u %u %u\n",hstart_min, vstart_min, hstop_max, vstop_max);
#endif
    }

    vstart_min_tracking = vstart_min;
    vstop_max_tracking = vstop_max;
    hstart_min_tracking = hstart_min;
    hstop_max_tracking = hstop_max;

#ifdef wasm_worker
    emscripten_wasm_worker_post_function_v(EMSCRIPTEN_WASM_WORKER_ID_PARENT,set_viewport_dimensions);
#else
    set_viewport_dimensions();
#endif

    reset_calibration=true;
  }
  else if(msg.type == Msg::VIDEO_FORMAT)
  {
    if(log_on) printf("video format=%s data1=%d\n",TVEnum::key(TV(msg.value)),data1);

    EM_ASM({use_ntsc_pixel= $0==1?true:false},TV(msg.value) == TV::NTSC);
    wasm_set_display(TV(msg.value) == TV::NTSC? "ntsc":"pal");
    request_to_reset_calibration=true;
  }
  else if(msg.type == Msg::DRIVE_STEP || msg.type == Msg::DRIVE_POLL 
     ||msg.type == Msg::HDR_STEP)
  {
      data1=msg.drive.nr;
      data2=msg.drive.value;
  }
  
  if(msg.type == Msg::DRIVE_SELECT)
  {
  }
  else
  {
    const char *message_as_string =  (const char *)MsgEnum::key(Msg(msg.type));

    if(msg.type == Msg::SER_OUT)
    {
      int byte = ((VAmiga *)emu)->serialPort.serialPort->readOutgoingByte();
      while(byte>=0)
      {
        send_message_to_js_with_param(message_as_string, byte, data2);
        byte = ((VAmiga *)emu)->serialPort.serialPort->readOutgoingByte();
      }
    }
    else
    {
      send_message_to_js_with_param(message_as_string, data1, data2);
    }
  }
}




class vAmigaWrapper {
  public:
    VAmiga *emu;

  vAmigaWrapper()
  {
    printf("constructing vAmiga ...\n");
    this->emu = new VAmiga();
  }
  ~vAmigaWrapper()
  {
        printf("closing wrapper");
  }

  void run()
  {
    try { emu->isReady(); } catch(...) { 
      printf("***** put missing rom message\n");
       // amiga->msgQueue.put(ROM_MISSING); 
        EM_ASM({
          setTimeout(function() {message_handler( 'MSG_ROM_MISSING' );}, 0);
        });
    }
    
    printf("wrapper calls run on vAmiga->run() method\n");


    emu->defaults.defaults->setFallback(Opt::HDC_CONNECT, false, {0});

//  wrapper->emu->defaults.setFallback(OPT_FILTER_TYPE, FILTER_NONE);
//  wrapper->emu->set(OPT_FILTER_TYPE, FILTER_NONE);

  // master Volumne
    emu->set(Opt::AUD_VOLL, 100); 
    emu->set(Opt::AUD_VOLR, 100);

  //Volumne
//  wrapper->emu->set(OPT_AUD_VOL0, 100); why did I set it only on channel 0? 
//  wrapper->emu->set(OPT_AUD_PAN0, 0);


  emu->set(Opt::MEM_CHIP_RAM, 512);
  emu->set(Opt::MEM_SLOW_RAM, 512);
  emu->set(Opt::AGNUS_REVISION, (i64)AgnusRevision::OCS);

  //turn automatic hd mounting off because kick1.2 makes trouble
  emu->set(Opt::HDC_CONNECT, false, /*hd drive*/ {0});

  emu->set(Opt::DRIVE_CONNECT,true, /*df1*/ {1});

  emu->emu->update();





    printf("waiting on emulator ready in javascript ...\n");
 
  }
};

#ifdef wasm_worker
emscripten_wasm_worker_t worker;

void run_in_worker()
{
//  EM_ASM({console.log("Hello log")});
//  emscripten_wasm_worker_post_function_sig(EMSCRIPTEN_WASM_WORKER_ID_PARENT,(void *) test_success, "id");
  auto behind = wasm_draw_one_frame(emscripten_performance_now());
  while(behind>0)
  {
    emu->execute();
    executed_since_last_host_frame++;
    executed_frame_count++;
    total_executed_frame_count++;
    behind--;
  }
}
#endif

extern "C" bool wasm_is_worker_built()
{
#ifdef wasm_worker
  return true;
#else
  return false;
#endif
}

extern "C" void wasm_worker_run()
{
  #ifdef wasm_worker
    emscripten_wasm_worker_post_function_v(worker, run_in_worker); 
  #endif
}

vAmigaWrapper *wrapper = NULL;
int main(int argc, char** argv) {
#ifdef wasm_worker
  worker = emscripten_malloc_wasm_worker(/*stack size: */2048);
  printf("running on wasm webworker...\n");
#else
  printf("running on main thread...\n");
#endif
  wrapper= new vAmigaWrapper();

  printf("connecting listener to vAmiga message queue...\n");

//  try{
      wrapper->emu->launch(wrapper->emu, &theListener);
//  } catch(std::exception &exception) {
//      printf("%s\n", exception.what());
//  }
//  printf("launch completed\n");

  wrapper->run();
  return 0;
}


extern "C" Texel * wasm_pixel_buffer()
{
//  auto stable_ptr = emu->denise.denise->pixelEngine.stablePtr();
  auto stable_ptr = emu->emu->getTexture().pixels.ptr;
  return stable_ptr;
}
extern "C" u32 wasm_frame_info()
{
//  auto &stableBuffer = emu->denise.denise->pixelEngine.getStableBuffer();
  auto &stableBuffer = emu->emu->getTexture();
  u32 info = (u32)stableBuffer.nr;
  info = info<<1;

  if(stableBuffer.prevlof)
    info |= 0x0001;
  
  info = info<<1;
  
  if(stableBuffer.lof)
    info |= 0x0001;
  
  return info;
}


extern "C" int wasm_get_renderer()
{ 
  return render_method;
}

extern "C" int wasm_get_render_width()
{ 
  return clipped_width;
}
extern "C" int wasm_get_render_height()
{ 
  return clipped_height;
}

extern "C" void wasm_set_target_fps(int _target_fps)
{ 
  target_fps=_target_fps;
}


/* emulation of macos mach_absolute_time() function. */
uint64_t mach_absolute_time()
{
    uint64_t nano_now = (uint64_t)(emscripten_get_now()*1000000.0);
    //printf("emsdk_now: %lld\n", nano_now);
    return nano_now; 
}

extern "C" void wasm_key(int code, int pressed)
{
//  printf("wasm_key ( %d, %d ) \n", code, pressed);

  if(pressed==1)
  {
//    wrapper->emu->keyboard.keyboard->pressKey(code);
      wrapper->emu->keyboard.keyboard->press(KeyCode(code));
  }
  else
  {
    wrapper->emu->keyboard.release(KeyCode(code));
  }
  wrapper->emu->emu->update();

}

extern "C" void wasm_auto_type(int code, int duration, int delay)
{//obsolete
//    if(log_on) printf("auto_type ( %d, %d, %d ) \n", code, duration, delay);
//    wrapper->emu->keyboard.autoType(code, MSEC(duration), MSEC(delay));
}


extern "C" void wasm_schedule_key(int code1, int code2, int pressed, int frame_delay)
{
  if(pressed==1)
  {
//    printf("scheduleKeyPress ( %d, %d, %d ) \n", code1, code2, frame_delay);
    wrapper->emu->keyboard.press(KeyCode(code1));
//    wrapper->emu->keyboard.scheduleKeyPress(*new AmigaKey(code1,code2), frame_delay);
  }
  else
  {
//    printf("scheduleKeyRelease ( %d, %d, %d ) \n", code1, code2, frame_delay);
    wrapper->emu->keyboard.release(KeyCode(code1));
  
  //  wrapper->emu->keyboard.scheduleKeyRelease(*new C64Key(code1,code2), frame_delay);
  }
  wrapper->emu->emu->update();

}





char wasm_pull_user_snapshot_file_json_result[255];


extern "C" bool wasm_has_disk(const char *drive_name)
{
  if(strcmp(drive_name,"df0") == 0)
  {
    return wrapper->emu->df0.getInfo().hasDisk;
  }
  else if(strcmp(drive_name,"df1") == 0)
  {
    return wrapper->emu->df1.getInfo().hasDisk;
  }
  else if(strcmp(drive_name,"df2") == 0)
  {
    return wrapper->emu->df2.getInfo().hasDisk;
  }
  else if(strcmp(drive_name,"df3") == 0)
  {
    return wrapper->emu->df3.getInfo().hasDisk;
  }
  else if (strcmp(drive_name,"dh0") == 0)
  {
    return wrapper->emu->hd0.getInfo().hasDisk;
  }
  else if (strcmp(drive_name,"dh1") == 0)
  {
    return wrapper->emu->hd1.getInfo().hasDisk;
  }
  else if (strcmp(drive_name,"dh2") == 0)
  {
    return wrapper->emu->hd2.getInfo().hasDisk;
  }
  else if (strcmp(drive_name,"dh3") == 0)
  {
    return wrapper->emu->hd3.getInfo().hasDisk;
  }

  return false;
}

void eject_harddisk(int drive)
{
    auto runs = wrapper->emu->isRunning();
    wrapper->emu->powerOff();wrapper->emu->emu->update();
    wrapper->emu->set(Opt::HDC_CONNECT, false, /*hd drive*/ {drive});
    wrapper->emu->powerOn();
    if(runs) wrapper->emu->run();
}


extern "C" void wasm_eject_disk(const char *drive_name)
{
  if(strcmp(drive_name,"df0") == 0)
  {
    if(wrapper->emu->df0.getInfo().hasDisk)
      wrapper->emu->df0.ejectDisk();
  }
  else if(strcmp(drive_name,"df1") == 0)
  {
    if(wrapper->emu->df1.getInfo().hasDisk)
      wrapper->emu->df1.ejectDisk();
  }
  else if(strcmp(drive_name,"df2") == 0)
  {
    if(wrapper->emu->df2.getInfo().hasDisk)
      wrapper->emu->df2.ejectDisk();
  }
  else if(strcmp(drive_name,"df3") == 0)
  {
    if(wrapper->emu->df3.getInfo().hasDisk)
      wrapper->emu->df3.ejectDisk();
  }
  else if (strcmp(drive_name,"dh0") == 0)
  {
    if(wrapper->emu->hd0.getInfo().hasDisk)
    {
      eject_harddisk(0);
    }
  }
  else if (strcmp(drive_name,"dh1") == 0)
  {
    if(wrapper->emu->hd1.getInfo().hasDisk)
    {
      eject_harddisk(1);
    }
  }
  else if (strcmp(drive_name,"dh2") == 0)
  {
    if(wrapper->emu->hd2.getInfo().hasDisk)
    {
      eject_harddisk(2);
    }
  }
  else if (strcmp(drive_name,"dh3") == 0)
  {
    if(wrapper->emu->hd3.getInfo().hasDisk)
    {
      eject_harddisk(3);
    }
  }

}

DiskFile *export_disk=NULL;
extern "C" void wasm_delete_disk()
{
  if(export_disk!=NULL)
  {
    delete export_disk;
    export_disk=NULL;
    if(log_on) printf("disk memory deleted\n");
  }
}



extern "C" char* wasm_export_as_folder(const char *drive_name,  const char *path)
{
  HardDrive *hd=NULL;
  if (strcmp(drive_name,"dh0") == 0)
  {
    hd = &wrapper->emu->hd0.getDrive();
  }
  else if (strcmp(drive_name,"dh1") == 0)
  {
    hd = &wrapper->emu->hd1.getDrive();
  }
  else if (strcmp(drive_name,"dh2") == 0)
  {
    hd = &wrapper->emu->hd2.getDrive();
  }
  else if (strcmp(drive_name,"dh3") == 0)
  {
    hd = &wrapper->emu->hd3.getDrive();
  }

  if (hd != NULL)
  {
    try
    {
      auto fs = new MutableFileSystem(*hd,0);
      fs->exportFiles(path);
      auto hd_name = fs->getName();
      delete fs;
      sprintf(wasm_pull_user_snapshot_file_json_result, "%s",hd_name.c_str());
      return wasm_pull_user_snapshot_file_json_result;
    }
    catch (const AppError& e) {
      printf("Error exporting hard drive to folder - %s\n",e.what());
      EM_ASM(
      {
        alert(`Error exporting hard drive to folder - ${UTF8ToString($0)}`);
      }, e.what());    
    }
  }
  sprintf(wasm_pull_user_snapshot_file_json_result, "{\"error\":\"No hard drive found\"}");
  return wasm_pull_user_snapshot_file_json_result;
}

extern "C" char* wasm_export_disk(const char *drive_name, u16 capacity_in_mb=10, const char *hd_name="vAmiga HDF")
{
  wasm_delete_disk();
  Buffer<u8> *data=NULL;
  sprintf(wasm_pull_user_snapshot_file_json_result, "{\"size\": 0 }");

  if(strcmp(drive_name,"df0") == 0)
  {
    if(!wrapper->emu->df0.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }
    export_disk = new ADFFile(wrapper->emu->df0.getDisk());
    data=&(export_disk->data);
  }
  else if(strcmp(drive_name,"df1") == 0)
  {
    if(!wrapper->emu->df1.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }
    export_disk = new ADFFile(wrapper->emu->df1.getDisk());
    data=&(export_disk->data);
  }
  else if(strcmp(drive_name,"df2") == 0)
  {
    if(!wrapper->emu->df2.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }
    export_disk = new ADFFile(wrapper->emu->df2.getDisk());
    data=&(export_disk->data);
  }
  else if(strcmp(drive_name,"df3") == 0)
  {
    if(!wrapper->emu->df3.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }
    export_disk = new ADFFile(wrapper->emu->df3.getDisk());
    data=&(export_disk->data);
  }
  else if (strcmp(drive_name,"dh0") == 0)
  {
    if(!wrapper->emu->hd0.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }

    export_disk = new HDFFile(wrapper->emu->hd0.getDrive());
    data=&(export_disk->data);
  }
  else if (strcmp(drive_name,"dh1") == 0)
  {
    if(!wrapper->emu->hd1.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }

    export_disk = new HDFFile(wrapper->emu->hd1.getDrive());
    data=&(export_disk->data);
  }
  else if (strcmp(drive_name,"dh2") == 0)
  {
    if(!wrapper->emu->hd2.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }

    export_disk = new HDFFile(wrapper->emu->hd2.getDrive());
    data=&(export_disk->data);
  }
  else if (strcmp(drive_name,"dh3") == 0)
  {
    if(!wrapper->emu->hd3.getInfo().hasDisk)
    {
      return wasm_pull_user_snapshot_file_json_result;
    }

    export_disk = new HDFFile(wrapper->emu->hd3.getDrive());
    data=&(export_disk->data);
  }
  else if (strcmp(drive_name,"/imported_hd") == 0)
  {
    try {
    auto drive=new HardDrive(*wrapper->emu->amiga.amiga,-1);

    drive->init(MB(capacity_in_mb));
    drive->format(FSFormat::FFS, hd_name);
    drive->importFolder("/imported_hd");
    export_disk = new HDFFile(*drive);
    delete drive;
  
    data=&(export_disk->data);
  } catch (const AppError& e) {
    printf("Error importing into hard drive - %s\n",e.what());
    EM_ASM(
    {
      alert(`Error importing into hard drive - ${UTF8ToString($0)}`);
    }, e.what());    
  }

  }
  
  sprintf(wasm_pull_user_snapshot_file_json_result, "{\"address\":%lu, \"size\": %lu }",
    (unsigned long)data->ptr, data->size);

  printf("return => %s\n",wasm_pull_user_snapshot_file_json_result);

  return wasm_pull_user_snapshot_file_json_result;
}


MediaFile *snapshot=NULL;
extern "C" void wasm_delete_user_snapshot()
{
//  printf("request to free user_snapshot memory\n");

  if(snapshot!=NULL)
  {
    delete snapshot;
    snapshot=NULL;
    if(log_on) printf("freed user_snapshot memory\n");
  }
}

extern "C" char* wasm_take_user_snapshot()
{
  try{
    printf("wasm_pull_user_snapshot_file\n");

    wasm_delete_user_snapshot();
    snapshot = wrapper->emu->amiga.takeSnapshot(); //wrapper->emu->userSnapshot(nr);

  //  printf("got snapshot %u.%u.%u\n", snapshot->getHeader()->major,snapshot->getHeader()->minor,snapshot->getHeader()->subminor );
    u8 *data = (u8*)(((Snapshot *)snapshot)->getHeader());

    printf("data header bytes= %x, %x, %x\n", data[0],data[1],data[2]);
  



    sprintf(wasm_pull_user_snapshot_file_json_result, "{\"address\":%lu, \"size\": %lu, \"width\": %lu, \"height\":%lu }",
    (unsigned long)data,//snapshot->getData(), 
    snapshot->getSize(),
    snapshot->previewImageSize().first,
    snapshot->previewImageSize().second
    );
    printf("return => %s\n",wasm_pull_user_snapshot_file_json_result);

    return wasm_pull_user_snapshot_file_json_result;
  } catch (const AppError& e) {
    printf("Error taking user snapshot - %s\n",e.what());
    EM_ASM(
    {
      alert(`Error taking user snapshot - ${UTF8ToString($0)}`);
    }, e.what());    
  }
  return wasm_pull_user_snapshot_file_json_result;
  
}


float sound_buffer[16384 * 2];
extern "C" float* wasm_get_sound_buffer_address()
{
  return sound_buffer;
}

extern "C" unsigned wasm_copy_into_sound_buffer()
{
  auto count=wrapper->emu->audioPort.port->stream.count();
  
  auto copied_samples=0;
  for(unsigned ipos=1024;ipos<=count;ipos+=1024)
  {
    wrapper->emu->audioPort.copyStereo(
    sound_buffer+copied_samples,
     sound_buffer+copied_samples+1024, 
     1024); 
    copied_samples+=1024*2;
//  printf("fillLevel (%lf)",wrapper->emu->paula.muxer.stream.fillLevel());
  }
  sum_samples += copied_samples/2; 

/*  printf("copyMono[%d]: ", 16);
  for(int i=0; i<16; i++)
  {
    //printf("%hhu,",stream[i]);
    printf("%f,",sound_buffer[i]);
  }
  printf("\n"); 
*/
  return copied_samples/2;
}


extern "C" void wasm_set_warp(unsigned on)
{
  wrapper->emu->set(Opt::AMIGA_WARP_MODE,(i64) (on == 1 ?Warp::AUTO:Warp::NEVER)); 
}


extern "C" bool wasm_is_warping()
{
  return wrapper->emu->isWarping();
}




extern "C" void wasm_set_display(const char *name)
{
  if(log_on) printf("wasm_set_display('%s')\n",name);

  if( strcmp(name,"ntsc") == 0)
  {
    name= display_names[geometry];
    if(log_on) printf("resetting new display %s\n",name);
    if(!ntsc)
    {
      if(log_on) printf("was not yet ntsc\n");

      if(wrapper->emu->get(Opt::AMIGA_VIDEO_FORMAT)!= (i64)TV::NTSC)
      {
        if(log_on) printf("was not yet ntsc so we have to configure it\n");
        wrapper->emu->set(Opt::AMIGA_VIDEO_FORMAT, (i64)TV::NTSC);
      }
      target_fps=NTSC_FPS;
      total_executed_frame_count=0;
      ntsc=true;
    }
  }
  else if( strcmp(name,"pal") == 0)
  {
    name= display_names[geometry];
    if(log_on) printf("resetting  new display %s\n",name);
    if(ntsc)
    {
      if(log_on) printf("was not yet PAL\n");
      if(wrapper->emu->get(Opt::AMIGA_VIDEO_FORMAT)!=(i64) TV::PAL)
      {
        if(log_on) printf("was not yet PAL so we have to configure it\n");
        wrapper->emu->set(Opt::AMIGA_VIDEO_FORMAT, (i64)TV::PAL);
      }
      target_fps=PAL_FPS;
      total_executed_frame_count=0;
      ntsc=false;
    }
  }
  else if( strcmp(name,"") == 0)
  {
    name= display_names[geometry];
    if(log_on) printf("reset display=%s\n",name);
  }


  if( strcmp(name,"adaptive") == 0 || 
      strcmp(name,"auto") == 0 || 
      strcmp(name,"viewport tracking") == 0)
  {
    geometry=DISPLAY_ADAPTIVE;
    wrapper->emu->set(Opt::DENISE_VIEWPORT_TRACKING, true); 
  }
  else if( strcmp(name,"borderless") == 0)
  {
    geometry=DISPLAY_BORDERLESS;
    wrapper->emu->set(Opt::DENISE_VIEWPORT_TRACKING, true); 
    return;
  }
  else if( strcmp(name,"narrow") == 0)
  {
    wrapper->emu->set(Opt::DENISE_VIEWPORT_TRACKING, false); 
    geometry=DISPLAY_NARROW;
    xOff=252 + 4;
    yOff=PAL::VBLANK_CNT +16;
    clipped_width=HPIXELS-xOff - 8;   
    //clipped_height=312-yOff -2*24 -2; 
    clipped_height=(3*clipped_width/4 +(ntsc?0:32) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL;}
  }
  else if( strcmp(name,"standard") == 0)
  {
    wrapper->emu->set(Opt::DENISE_VIEWPORT_TRACKING, false); 
  
    geometry=DISPLAY_STANDARD;
    xOff=208+HBLANK_MAX;
    yOff=PAL::VBLANK_CNT +10;
    clipped_width=HPIXELS-xOff;
//    clipped_height=312-yOff -2*4  ;
//    clipped_height=(4*clipped_width/5 )/2 & 0xfffe;
    clipped_height=(3*clipped_width/4 +(ntsc?0:32) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL-10;}
  }
  else if( strcmp(name,"wider") == 0)
  {
    wrapper->emu->set(Opt::DENISE_VIEWPORT_TRACKING, false); 
  
    geometry=DISPLAY_WIDER;
    xOff=208+ HBLANK_MAX/2;
    yOff=PAL::VBLANK_CNT + 2;
    clipped_width=(HPIXELS+HBLANK_MAX/2 )-xOff;
//    clipped_height=312-yOff -2*2;
    clipped_height=(3*clipped_width/4 +(ntsc?0:32) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL-8;}
  }
  else if( strcmp(name,"overscan") == 0)
  {
    wrapper->emu->set(Opt::DENISE_VIEWPORT_TRACKING, false); 
  
    geometry=DISPLAY_OVERSCAN;

    xOff=208; //208 is first pixel in dpaint iv,overscan=max
    yOff=PAL::VBLANK_CNT; //must be even
    clipped_width=(HPIXELS+HBLANK_MAX)-xOff;
    //clipped_height=312-yOff; //must be even
    clipped_height=(3*clipped_width/4 +(ntsc?0:24) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL;}
  } 
  else if( strcmp(name,"extreme") == 0)
  {
    wrapper->emu->set(Opt::DENISE_VIEWPORT_TRACKING, false); 
  
    geometry=DISPLAY_EXTREME;

    xOff=208-48; //208 is first pixel in dpaint iv,overscan=max
    yOff=ntsc?NTSC::VBLANK_CNT:PAL::VBLANK_CNT; //must be even
    clipped_width=(HPIXELS+HBLANK_MAX)-xOff;
    //clipped_height=312-yOff; //must be even
    if(ntsc)
    {
      clipped_height=(NTSC::VPOS_MAX - yOff) & 0xfffe;
    }
    else
    {
      clipped_height=(PAL::VPOS_MAX - yOff) & 0xfffe;
    }
  }
  
  if(log_on) printf("width=%d, height=%d, ratio=%f\n", clipped_width, clipped_height, (float)clipped_width/(float)clipped_height);

  EM_ASM({js_set_display($0,$1,$2,$3); scaleVMCanvas();},xOff, yOff, clipped_width*TPP,clipped_height );
}

std::unique_ptr<FloppyDisk> load_disk(const char* filename, u8 *blob, long len)
{
  printf("file content starts with %.*s\n",8, blob );

  if (DMSFile::isCompatible(filename)) {
    printf("%s - Loading DMS file\n", filename);
    DMSFile dms{blob, len};
    return std::make_unique<FloppyDisk>(dms);
  }

  if (strcmp((char*)blob, "UAE--ADF")==0 || strcmp((char*)blob, "UAE-1ADF")==0) {
      printf("compatible extadf\n");
      EADFFile ext{blob, len};
      return std::make_unique<FloppyDisk>(ext);
  }

  if (ADFFile::isCompatible(filename)) {
    printf("%s - Loading ADF file\n", filename);
    ADFFile adf{blob, len};
    return std::make_unique<FloppyDisk>(adf);
  }

  if (EXEFile::isCompatible(filename)) {
    printf("%s - Loading EXE file\n", filename);
    EXEFile exe{blob, len};
    return std::make_unique<FloppyDisk>(exe);
  }
  if (STFile::isCompatible(filename)) {
    printf("%s - Loading ST file\n", filename);
    STFile st{blob, len};
    return std::make_unique<FloppyDisk>(st);
  }
  if (OtherFile::isCompatible(filename)) {
    if(len > 1710000)
    { 
      EM_ASM(
      {
        alert(`Error loading ${UTF8ToString($0)} to disk - sorry, only files below 1.71MB can be mounted as disk.`);
      }, filename);
    }
    else {
      printf("%s - import as disk\n", filename);
      OtherFile other{filename,blob, len};
      return std::make_unique<FloppyDisk>(other);
    }
  }
  return {};
}


extern "C" const void wasm_mem_patch(u32 amiga_mem_address, u8 *blob, isize len)
{
  printf("wasm_mem_patch addr=0x%x, len=%ld, header bytes= %x, %x, %x\n", amiga_mem_address, len, blob[0],blob[1],blob[2]);
  if(wrapper == NULL) return;

  wrapper->emu->mem.mem->patch(amiga_mem_address, blob, len);
  return;
}

string
extractSuffix(const string &s)
{
    auto idx = s.rfind('.');
    auto pos = idx != string::npos ? idx + 1 : 0;
    auto len = string::npos;
    return s.substr(pos, len);
}

extern "C" const char* _wasm_loadFile(char* name, u8 *blob, long len, u8 drive_number)
{
  printf("load drive=%d, file=%s len=%ld, header bytes= %x, %x, %x\n", drive_number, name, len, blob[0],blob[1],blob[2]);
  filename=name;
  if(wrapper == NULL)
  {
    return "";
  }
  try{
    if (auto disk = load_disk(name, blob, len)) {
      if(drive_number>0)
      {//configure correct disk drive type (df0 does only accept DD, no HD)
        wrapper->emu->set(Opt::DRIVE_TYPE, (i64)(disk->density==Density::DD? FloppyDriveType::DD_35:FloppyDriveType::HD_35), {drive_number} );
        wrapper->emu->emu->update();
      }

      if(drive_number==0){
        if(disk->density == Density::DD)
        {
          wrapper->emu->df0.drive->swapDisk(std::move(disk));
        }
        else
          EM_ASM(
          {
            let_drive_select_stay_open=true;
            alert(`'${UTF8ToString($0)}' is a HD disk which is not compatible with df0. AmigaOS only supports DD disks in df0, please mount disk in df1 - df3, which beside DD also support HD disks.`);
          }, filename);
      }
      else if(drive_number==1)
        wrapper->emu->df1.drive->swapDisk(std::move(disk));
      else if(drive_number==2)
        wrapper->emu->df2.drive->swapDisk(std::move(disk));
      else if(drive_number==3)
        wrapper->emu->df3.drive->swapDisk(std::move(disk));

      return "";
    }
  } catch (const AppError& e) {
    printf("Error loading %s - %s\n", filename, e.what());
    EM_ASM(
    {
      alert(`Error loading ${UTF8ToString($0)} - ${UTF8ToString($1)}`);
    }, filename, e.what());    
  }

  if (HDFFile::isCompatible(filename)) 
  {
    printf("is hdf\n");

    HDFFile *hdf;

    try{    
      hdf = new HDFFile(blob, len);
    }
    catch(AppError &exception) {
      printf("Failed to create HDF image file %s\n", name);
      Fault ec=Fault(exception.data);
      printf("%s - %s\n", FaultEnum::key(ec), exception.what());
      EM_ASM(
      {
        alert(`${UTF8ToString($0)} - ${UTF8ToString($1)}`);
      }, FaultEnum::key(ec), exception.what());
      return FaultEnum::key(ec); 
    }    

    auto hard_drive = wrapper->emu->hd0.drive;
    if(drive_number==1)
    {
      hard_drive = wrapper->emu->hd1.drive;
    }
    else if(drive_number==2)
    {
      hard_drive = wrapper->emu->hd2.drive;
    }
    else if(drive_number==3)
    {
      hard_drive = wrapper->emu->hd3.drive;
    }

    try
    {
        hard_drive->init(*hdf);
        if(!hard_drive->getInfo().hasDisk)
        {
          throw Fault(Fault::OUT_OF_MEMORY);
        }
    }
    catch(AppError &exception) {
      printf("Failed to init HDF image file %s\n", name);
      EM_ASM(
      {
        alert(`${UTF8ToString($0)}`);
      }, exception.what());
      delete hdf;
      return exception.what();
    }
 
    delete hdf;

    wrapper->emu->powerOff(); wrapper->emu->emu->update();
    wrapper->emu->set(Opt::HDC_CONNECT, true, {drive_number});
    wrapper->emu->emu->update();
    wrapper->emu->powerOn(); //does set emu in paused mode
    wrapper->emu->run(); //needed otherwise core will stay muted if it was paused
    wrapper->emu->emu->update();
    return "";
  }
  bool file_still_unprocessed=true;
  if (Snapshot::isCompatible(blob,len) && extractSuffix(filename)!="rom")
  {  
    try
    {
      if(log_on) printf("try to build Snapshot\n");
      Snapshot *file = new Snapshot(blob, len);      
      printf("isSnapshot\n");
      wrapper->emu->amiga.loadSnapshot(*file);
      file_still_unprocessed=false;
      delete file;
//      wasm_set_display(wrapper->emu->agnus.isNTSC()?"ntsc":"pal");

/*      if(geometry==DISPLAY_BORDERLESS || geometry == DISPLAY_ADAPTIVE)
      {//it must determine the viewport again, i.e. we need a new message for calibration
        //enforce this by calling
        wrapper->emu->set(OPT_VIEWPORT_TRACKING, true); 
      }
*/
      printf("run snapshot at %f Hz, isPAL=%d\n", target_fps, !ntsc);
    }
    catch(AppError &exception) {
      Fault ec=Fault(exception.data);
      printf("%s\n", FaultEnum::key(ec));
    }
  }

  if(file_still_unprocessed && extractSuffix(filename)=="rom_file")
  {
    bool wasRunnable = true;
    try { wrapper->emu->isReady(); } catch(...) { wasRunnable=false; }

    RomFile *rom = NULL;
    try
    {
      printf("try to build RomFile\n");
      rom = new RomFile(blob, len);
    }
    catch(AppError &exception) {
      printf("Failed to read ROM image file %s\n", name);
      Fault ec=Fault(exception.data);
      printf("%s\n", FaultEnum::key(ec));
      return "";
    }

    if(wrapper->emu->isPoweredOn())
    {
      wrapper->emu->powerOff(); wrapper->emu->emu->update();
    }
//    wrapper->emu->suspend();
    try { 
      wrapper->emu->mem.loadRom(*rom); 
      
      printf("Loaded ROM image %s. %s\n", name, wrapper->emu->mem.getRomTraits().title);
      if(strncmp("EmuTOS",wrapper->emu->mem.getRomTraits().title,strlen("EmuTOS"))==0)
      {
        printf("detected EmuTOS rom, setting drive speed to -1\n");
        wrapper->emu->set(Opt::DC_SPEED, -1);
      }
/*      wrapper->emu->set(OPT_HDC_CONNECT,
        //hd drive
        0, 
        //enable if not AROS
        strcmp(wrapper->emu->mem.romTitle(),"AROS Kickstart replacement")!=0
      );
*/
    }  
    catch(AppError &exception) { 
      printf("Failed to flash ROM image %s.\n", name);
      Fault ec=Fault(exception.data);
      printf("%s\n", FaultEnum::key(ec));
      return "";
    }
    const char *rom_type="rom";
    try
    {
        bool is_ready_now = true;
        try { wrapper->emu->isReady(); } catch(...) { is_ready_now=false; }

        if (!wasRunnable && is_ready_now)
        {
          printf("was not runnable is ready now (rom)\n");
          wrapper->emu->powerOn();
        
          //wrapper->emu->putMessage(MSG_READY_TO_RUN);
          const char* ready_msg= "READY_TO_RUN";
          printf("sending ready message %s.\n", ready_msg);
          send_message_to_js(ready_msg);    
        }

        delete rom;
        wrapper->emu->powerOn();
        wrapper->emu->run();
    }    
    catch(AppError &exception) { 
      Fault ec=Fault(exception.data);
      printf("%s\n", FaultEnum::key(ec));
    } 

    return rom_type;    
  }
  
  if(file_still_unprocessed && extractSuffix(filename)=="rom_ext_file")
  {
    bool wasRunnable = true;
    try { wrapper->emu->isReady(); } catch(...) { wasRunnable=false; }

    RomFile *rom = NULL;
    try
    {
      printf("try to build RomFile\n");
      rom = new RomFile(blob, len);
    }
    catch(AppError &exception) {
      printf("Failed to read ROM_EXT image file %s\n", name);
      Fault ec=Fault(exception.data);
      printf("%s\n", FaultEnum::key(ec));
      return "";
    }

    auto was_running = wrapper->emu->isRunning();
    if(wrapper->emu->isPoweredOn())
    {
      wrapper->emu->powerOff(); wrapper->emu->emu->update();
    }
    //wrapper->emu->suspend();
    try { 
      wrapper->emu->mem.loadExt(*rom); 
      
      printf("Loaded ROM_EXT image %s.\n", name);
      
    }  
    catch(AppError &exception) { 
      printf("Failed to flash ROM_EXT image %s.\n", name);
      Fault ec=Fault(exception.data);
      printf("%s\n", FaultEnum::key(ec));
    }
  

    bool is_ready_now = true;
    try { wrapper->emu->isReady(); } catch(...) { is_ready_now=false; }

    if (!wasRunnable && is_ready_now)
    {
      printf("was not runnable is ready now (romext)\n");
      wrapper->emu->powerOn();

       //wrapper->emu->putMessage(MSG_READY_TO_RUN);
      const char* ready_msg= "READY_TO_RUN";
      printf("sending ready message %s.\n", ready_msg);
      send_message_to_js(ready_msg);    
    }

    const char *rom_type="rom_ext";
    delete rom;
    wrapper->emu->powerOn();
    if(was_running) wrapper->emu->run();
    return rom_type;    
  }

  return "";
}


extern "C" const char* wasm_loadFile(char* name, u8 *blob, long len, u8 drive_number)
{
  try
  {
    return _wasm_loadFile(name, blob, len, drive_number);
  }
  catch (const AppError& e) {
    EM_ASM(
    {
      alert(`Error loading ${UTF8ToString($0)} - ${UTF8ToString($1)}`);
    }, name, e.what());    
  }
  return "";
}

extern "C" void wasm_reset()
{
  wrapper->emu->hardReset();
}


extern "C" void wasm_halt()
{
  printf("wasm_halt\n");
  wrapper->emu->pause();

//  printf("emscripten_pause_main_loop() at MSG_PAUSE\n");
//  paused_the_emscripten_main_loop=true;
  //emscripten_pause_main_loop();
//  printf("after emscripten_set_main_loop_arg() at MSG_RUN\n");

}

extern "C" void wasm_run()
{
  if(log_on) printf("wasm_run\n");
  
  if(log_on) printf("is running = %u\n",wrapper->emu->isRunning());

  wrapper->emu->run();
  emu=wrapper->emu;
/*
  if(paused_the_emscripten_main_loop || already_run_the_emscripten_main_loop)
  {
    if(log_on) printf("emscripten_resume_main_loop at MSG_RUN %u, %u\n", paused_the_emscripten_main_loop, already_run_the_emscripten_main_loop);
//    emscripten_resume_main_loop();
  }
  else
  {
    if(log_on) printf("emscripten_set_main_loop_arg() at MSG_RUN %u, %u\n", paused_the_emscripten_main_loop, already_run_the_emscripten_main_loop);
    already_run_the_emscripten_main_loop=true;
    thisAmiga=wrapper->emu;
    //emscripten_set_main_loop_arg(draw_one_frame_into_SDL, (void *)wrapper->emu, 0, 1);
    if(log_on) printf("after emscripten_set_main_loop_arg() at MSG_RUN\n");
  }
*/
}


extern "C" void wasm_mouse(int port, int x, int y)
{
  //printf("wasm_mouse port%d x=%d, y=%d\n", port, x, y);

  /*if(port==1)
    wrapper->emu->controlPort1.mouse->setDxDy(x,y); 
  else if(port==2)
    wrapper->emu->controlPort2.mouse->setDxDy(x,y);
  */
  wrapper->emu->put(Cmd::MOUSE_MOVE_REL, CoordCmd(port-1, x, y));
}

extern "C" void wasm_mouse_button(int port, int button_id, int pressed)
{ 
    if(button_id==1)
      wrapper->emu->put(Cmd::MOUSE_BUTTON, GamePadCmd(port-1, (pressed==1?GamePadAction::PRESS_LEFT:GamePadAction::RELEASE_LEFT)));
      //wrapper->emu->put(CMD_MOUSE_EVENT, GamePadCmd(port-1,(pressed==1?PRESS_LEFT:RELEASE_LEFT)));
    else if(button_id==2)
    wrapper->emu->put(Cmd::MOUSE_BUTTON, GamePadCmd(port-1, (pressed==1?GamePadAction::PRESS_MIDDLE:GamePadAction::RELEASE_MIDDLE)));
    //      wrapper->emu->put(CMD_MOUSE_EVENT, GamePadCmd(port-1,(pressed==1?PRESS_MIDDLE:RELEASE_MIDDLE)));
    else if(button_id==3)
    //      wrapper->emu->put(CMD_MOUSE_EVENT, GamePadCmd(port-1,(pressed==1?PRESS_RIGHT:RELEASE_RIGHT)));
    wrapper->emu->put(Cmd::MOUSE_BUTTON, GamePadCmd(port-1, (pressed==1?GamePadAction::PRESS_RIGHT:GamePadAction::RELEASE_RIGHT)));

}

extern "C" void wasm_joystick(char* port_plus_event)
{
//    printf("wasm_joystick event=%s\n", port_plus_event);
  /*
  from javascript
    // port, o == oben, r == rechts, ..., f == feuer
    //states = '1', '1o', '1or', '1r', '1ur', '1u', '1ul', '1l', '1ol' 
    //states mit feuer = '1f', '1of', '1orf', '1rf', '1urf', ...
  */
/*
outgoing
PULL_UP
PRESS_FIRE

RELEASE_X
RELEASE_Y
RELEASE_XY
RELEASE_FIRE
*/
  char joyport = port_plus_event[0];
  char* event  = port_plus_event+1;

  GamePadAction code;
  if( strcmp(event,"PULL_UP") == 0)
  {
    code = GamePadAction::PULL_UP;
  }
  else if( strcmp(event,"PULL_LEFT") == 0)
  {
    code = GamePadAction::PULL_LEFT;
  }
  else if( strcmp(event,"PULL_DOWN") == 0)
  {
    code = GamePadAction::PULL_DOWN;
  }
  else if( strcmp(event,"PULL_RIGHT") == 0)
  {
    code = GamePadAction::PULL_RIGHT;
  }
  else if( strcmp(event,"PRESS_FIRE") == 0)
  {
    code = GamePadAction::PRESS_FIRE;
  }
  else if( strcmp(event,"PRESS_FIRE2") == 0)
  {
    code = GamePadAction::PRESS_FIRE2;
  }
  else if( strcmp(event,"PRESS_FIRE3") == 0)
  {
    code = GamePadAction::PRESS_FIRE3;
  }
  else if( strcmp(event,"RELEASE_XY") == 0)
  {
    code = GamePadAction::RELEASE_XY;
  }
  else if( strcmp(event,"RELEASE_X") == 0)
  {
    code = GamePadAction::RELEASE_X;
  }
  else if( strcmp(event,"RELEASE_Y") == 0)
  {
    code = GamePadAction::RELEASE_Y;
  }
  else if( strcmp(event,"RELEASE_FIRE") == 0)
  {
    code = GamePadAction::RELEASE_FIRE;
  }
  else if( strcmp(event,"RELEASE_FIRE2") == 0)
  {
    code = GamePadAction::RELEASE_FIRE2;
  }
  else if( strcmp(event,"RELEASE_FIRE3") == 0)
  {
    code = GamePadAction::RELEASE_FIRE3;
  }
  else
  {
    return;    
  }

  if(joyport == '1')
  {
    wrapper->emu->controlPort1.joystick.trigger(code);
  }
  else if(joyport == '2')
  {
    wrapper->emu->controlPort2.joystick.trigger(code);
  }

}

char buffer[50];
extern "C" char* wasm_sprite_info()
{
  if(!wrapper->emu->isTracking())
  {
    wrapper->emu->trackOn();
  }
//   wrapper->emu->setInspectionTarget(INSPECTION_DENISE, MSEC(250));
//   wrapper->emu->denise.debugger.recordSprite(0);

   Denise *denise = wrapper->emu->denise.denise;
   auto spriteinfo0 = denise->debugger.getSpriteInfo(0);
   auto spriteinfo1 = denise->debugger.getSpriteInfo(1);
   auto spriteinfo2 = denise->debugger.getSpriteInfo(2);
   auto spriteinfo3 = denise->debugger.getSpriteInfo(3);
   auto spriteinfo4 = denise->debugger.getSpriteInfo(4);
   auto spriteinfo5 = denise->debugger.getSpriteInfo(5);
   auto spriteinfo6 = denise->debugger.getSpriteInfo(6);
   auto spriteinfo7 = denise->debugger.getSpriteInfo(7);


   sprintf(buffer, "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", 
     spriteinfo0.hstrt*2,
     spriteinfo0.vstrt, 
     spriteinfo1.hstrt*2,
     spriteinfo1.vstrt, 
     spriteinfo2.hstrt*2,
     spriteinfo2.vstrt, 
     spriteinfo3.hstrt*2,
     spriteinfo3.vstrt, 
     spriteinfo4.hstrt*2,
     spriteinfo4.vstrt, 
     spriteinfo5.hstrt*2,
     spriteinfo5.vstrt, 
     spriteinfo6.hstrt*2,
     spriteinfo6.vstrt, 
     spriteinfo7.hstrt*2,
     spriteinfo7.vstrt
     );  

   return buffer;
}


extern "C" void wasm_cut_layers(unsigned cut_layers)
{
  wrapper->emu->set(Opt::DENISE_HIDDEN_LAYER_ALPHA,255);
//  wrapper->emu->set(OPT_HIDDEN_SPRITES, 0x100 | (SPR0|SPR1|SPR2|SPR3|SPR4|SPR5|SPR6|SPR7)); 
  wrapper->emu->set(Opt::DENISE_HIDDEN_LAYERS, cut_layers); 
}



char json_result[1024];
extern "C" const char* wasm_rom_info()
{

  sprintf(json_result, "{\"hasRom\":\"%s\",\"hasExt\":\"%s\", \"romTitle\":\"%s\", \"romVersion\":\"%s\", \"romReleased\":\"%s\", \"romModel\":\"%s\", \"extTitle\":\"%s\", \"extVersion\":\"%s\", \"extReleased\":\"%s\", \"extModel\":\"%s\" }",
    wrapper->emu->mem.mem->hasRom()?"true":"false",
    wrapper->emu->mem.mem->hasExt()?"true":"false",
    wrapper->emu->mem.getRomTraits().title,
    wrapper->emu->mem.getRomTraits().revision,
    wrapper->emu->mem.getRomTraits().released,
    wrapper->emu->mem.getRomTraits().model,
    wrapper->emu->mem.getExtTraits().title,
    wrapper->emu->mem.getExtTraits().revision,
    wrapper->emu->mem.getExtTraits().released,
    wrapper->emu->mem.getExtTraits().model
  );

/*
  printf("%s, %s, %s, %s\n",      wrapper->emu->mem.romTitle(),
    wrapper->emu->mem.romVersion(),
    wrapper->emu->mem.romReleased(),
    ""
//    wrapper->emu->mem.romModel()
  );
*/
  return json_result;
}

extern "C" const char* wasm_get_core_version()
{
  sprintf(json_result, "%s",
    wrapper->emu->version().c_str() 
  );

  return json_result;
}



extern "C" void wasm_set_color_palette(char* palette)
{

  if( strcmp(palette,"color") == 0)
  {
    wrapper->emu->set(Opt::MON_PALETTE, (i64)Palette::COLOR);
  }
  else if( strcmp(palette,"black white") == 0)
  { 
    wrapper->emu->set(Opt::MON_PALETTE, (i64)Palette::BLACK_WHITE); 
  }
  else if( strcmp(palette,"paper white") == 0)
  { 
    wrapper->emu->set(Opt::MON_PALETTE, (i64)Palette::PAPER_WHITE); 
  }
  else if( strcmp(palette,"green") == 0)
  { 
    wrapper->emu->set(Opt::MON_PALETTE, (i64)Palette::GREEN); 
  }
  else if( strcmp(palette,"amber") == 0)
  { 
    wrapper->emu->set(Opt::MON_PALETTE, (i64)Palette::AMBER); 
  }
  else if( strcmp(palette,"sepia") == 0)
  { 
    wrapper->emu->set(Opt::MON_PALETTE, (i64)Palette::SEPIA); 
  }

}


extern "C" u64 wasm_get_cpu_cycles()
{
  return wrapper->emu->cpu.cpu->getClock();
}

char config_result[512];
extern "C" const char* wasm_power_on(unsigned power_on)
{
  try{
    bool was_powered_on=wrapper->emu->isPoweredOn();
    if(power_on == 1 && !was_powered_on)
    {
        wrapper->emu->powerOn();
        wrapper->emu->run();
    }
    else if(power_on == 0 && was_powered_on)
    {
        wrapper->emu->powerOff();wrapper->emu->emu->update();
    }
  }  
  catch(AppError &exception) {   
    sprintf(config_result,"%s", exception.what());
  }
  return config_result; 
}


extern "C" void wasm_set_sample_rate(unsigned sample_rate)
{
    printf("set paula.muxer to freq= %d\n", sample_rate);

    wrapper->emu->set(Opt::HOST_SAMPLE_RATE,sample_rate);
    wrapper->emu->emu->update();
    auto got_sample_rate=wrapper->emu->get(Opt::HOST_SAMPLE_RATE);

    printf("amiga.host.getSampleRate()==%lld\n", got_sample_rate);
}



extern "C" i32 wasm_get_config_item(char* item_name, unsigned data)
{  
  if(strcmp(item_name,"DRIVE_CONNECT") == 0 )
  {
    return wrapper->emu->get(Opt::DRIVE_CONNECT,data);
  }
  else
  {
    return wrapper->emu->get(Opt(util::parseEnum <OptEnum>(std::string(item_name))));
  }
}

extern "C" const char* wasm_configure_key(char* option, char* key, char* _value)
{
 // printf("----->wasm_configure_key %s %s = %s\n", option, key, _value);
 // return config_result;
  sprintf(config_result,""); 
  auto value = std::string(_value);
  if(log_on) printf("wasm_configure_key %s %s = %s\n", option, key, value.c_str());

  try {
/*
    setFallback(OPT_DMA_DEBUG_ENABLE, false);
    setFallback(OPT_DMA_DEBUG_MODE, DMA_DISPLAY_MODE_FG_LAYER);
    setFallback(OPT_DMA_DEBUG_OPACITY, 50);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_COPPER, true);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_BLITTER, true);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_DISK, true);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_AUDIO, true);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_SPRITE, true);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_BITPLANE, true);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_CPU, false);
    setFallback(OPT_DMA_DEBUG_CHANNEL, DMA_CHANNEL_REFRESH, true);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_COPPER, 0xFFFF0000);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_BLITTER, 0xFFCC0000);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_DISK, 0x00FF0000);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_AUDIO, 0xFF00FF00);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_SPRITE, 0x0088FF00);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_BITPLANE, 0x00FFFF00);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_CPU, 0xFFFFFF00);
    setFallback(OPT_DMA_DEBUG_COLOR, DMA_CHANNEL_REFRESH, 0xFF000000);
*/
        wrapper->emu->set(Opt::DMA_DEBUG_ENABLE, true);   
        wrapper->emu->set(
         //OPT_DMA_DEBUG_CHANNEL6,
         Opt(util::parseEnum <OptEnum>(std::string(option))),
//         util::parseEnum <DmaChannelEnum>(std::string(key)), 
         util::parseBool(std::string(key))); 
      
  }
  catch(AppError &exception) {
      printf("unknown key %s %s = %s\n", option, key, value.c_str());

//    ErrorCode ec=exception.data;
//    sprintf(config_result,"%s", ErrorCodeEnum::key(ec));
    sprintf(config_result,"%s", exception.what());
  }
  return config_result; 
}



void calibrate_boost(signed boost_param){
      if(boost_param >4)
        return;
      vsync_speed=boost_param;
      unsigned boost = boost_param<0 ?
        host_refresh_rate / (boost_param*-1)
        :
        host_refresh_rate * (boost_param);
      vframes=0;
      speed_boost= ((double)boost / target_fps /*which is PAL_FPS or NTSC_FPS */);
      unsigned speed_boost_param=(unsigned)(speed_boost*100);
      printf("host_refresh_rate=%d, boost=%d, speed_boost=%lf, speed_param=%d\n",host_refresh_rate, boost, speed_boost, speed_boost_param);
      
      wrapper->emu->set(Opt::AMIGA_SPEED_BOOST,speed_boost_param );
      
      EM_ASM({$("#host_fps").html(`${$0} Hz`)},
        vsync_speed <0 ? host_refresh_rate/(vsync_speed*-1) : host_refresh_rate*vsync_speed );
}

extern "C" const char* wasm_configure(char* option, char* _value)
{  
  sprintf(config_result,""); 
  auto value = std::string(_value);
  if(log_on) printf("wasm_configure %s = %s\n", option, value.c_str());

  if(strcmp(option,"warp_to_frame") == 0 )
  {
    auto warp_to_frame= util::parseNum(value);
    wrapper->emu->set(Opt::AMIGA_WARP_BOOT, warp_to_frame/(wrapper->emu->agnus.agnus->isPAL()?50:60));
    wrapper->emu->emu->update();
    wrapper->emu->softReset(); //agnus.reset() schedules warp_off therefore we have to reset here after changing warp_boot 
    return config_result;
  }
  else if(strcmp(option,"log_on") == 0 )
  {
    log_on= util::parseBool(value);
    return config_result;
  }
  else if(strcmp(option,"floppy_drive_count") == 0 )
  {
    auto df_count= util::parseNum(value);
    int i=0;
    while(i<df_count)
    {
      wrapper->emu->set(Opt::DRIVE_CONNECT, true, {i});
      i++;
    }
    while(i<4)
    {
      wrapper->emu->set(Opt::DRIVE_CONNECT, false, {i});
      i++;
    }
    wrapper->emu->emu->update();
    return config_result;
  }

  bool was_powered_on=wrapper->emu->isPoweredOn();
  bool was_running =wrapper->emu->isRunning();


  bool must_be_off= strcmp(option,"AGNUS_REVISION") == 0 || 
                    strcmp(option,"DENISE_REVISION") == 0 ||
                    strcmp(option,"CHIP_RAM") == 0 ||
                    strcmp(option,"SLOW_RAM") == 0 ||
                    strcmp(option,"FAST_RAM") == 0 ||
                    strcmp(option,"CPU_REVISION") == 0;
 
  if(was_powered_on && must_be_off)
  {
      wrapper->emu->powerOff();wrapper->emu->emu->update();
  }

  try{
    if( strcmp(option,"AGNUS_REVISION") == 0)
    {
      wrapper->emu->set(Opt::AGNUS_REVISION, util::parseEnum <AgnusRevisionEnum>(value)); 
    }
    else if( strcmp(option,"DENISE_REVISION") == 0)
    {
      wrapper->emu->set(Opt::DENISE_REVISION, util::parseEnum <DeniseRevEnum>(value));
    }
    else if( strcmp(option,"WARP_MODE") == 0)
    {
      wrapper->emu->set(Opt::AMIGA_WARP_MODE, util::parseEnum <WarpEnum>(
        value.size() > 5 && value.substr(0, 5) == "WARP_" ? 
        value.substr(5) //legacy: some websites still use the WARP_ prefix variant 
        : 
        value
      ));
    }
    else if( strcmp(option,"SER_DEVICE") == 0)
    {
      wrapper->emu->set(Opt(util::parseEnum <OptEnum>(std::string(option))), util::parseEnum<SerialPortDeviceEnum>(value));
    }
    else if ( strcmp(option,"BLITTER_ACCURACY") == 0 ||
              strcmp(option,"DRIVE_SPEED") == 0  ||
              strcmp(option,"CHIP_RAM") == 0  ||
              strcmp(option,"SLOW_RAM") == 0  ||
              strcmp(option,"FAST_RAM") == 0  ||
              strcmp(option,"CPU_OVERCLOCKING") == 0 ||
              strcmp(option,"CPU_REVISION") == 0
    )
    {
      if(strcmp(option,"BLITTER_ACCURACY") == 0)
      {//TODO kann nicht so bleiben
        wrapper->emu->set(Opt::BLITTER_ACCURACY, util::parseNum(value));
      }
      else if(strcmp(option,"DRIVE_SPEED") == 0)
      {//TODO kann nicht so bleiben
        wrapper->emu->set(Opt::DC_SPEED, util::parseNum(value));
      }
      else if(strcmp(option,"CPU_REVISION") == 0)
      {//TODO kann nicht so bleiben
        wrapper->emu->set(Opt::CPU_REVISION, util::parseNum(value));
      }
      else if(strcmp(option,"CPU_OVERCLOCKING") == 0)
      {//TODO kann nicht so bleiben
        wrapper->emu->set(Opt::CPU_OVERCLOCKING, util::parseNum(value));
      }
      else
        wrapper->emu->set(Opt(util::parseEnum <OptEnum>(std::string(option))), util::parseNum(value));
    }
    else if ( strcmp(option,"DMA_DEBUG_CHANNEL") == 0 )
    {
//todo
      wrapper->emu->set(Opt(util::parseEnum <OptEnum>(std::string(option))),  util::parseBool(value));
    }
    else if(strcmp(option,"OPT_EMU_RUN_AHEAD") == 0)
    {
      auto frames=util::parseNum(value);
      printf("calling amiga->configure %s = %ld\n", option, frames);
      wrapper->emu->set(Opt::AMIGA_RUN_AHEAD, frames);
    }
    else if( strcmp(option,"OPT_AMIGA_SPEED_BOOST") == 0)
    {
      boost_param=(signed) util::parseNum(value);
      /* setting
        sync mode: { vsync x1/4=-4, ..., vsync=1, vsync x2=2, 50%=50, 75%=75, 100%, 150%, 200% }
      */
      if(boost_param <= 4)
      {
        vsync=true;
        calibrate_boost(boost_param);
      }
      else
      {
        vsync=false;
        wrapper->emu->set(Opt::AMIGA_SPEED_BOOST, boost_param);
        speed_boost= ((double) boost_param) / 100.0;
      }
      requested_targetFrameCount_reset=true;
    }
    else if(strcmp(option,"AUD.SAMPLING_METHOD") == 0)
    {
      wrapper->emu->set(Opt::AUD_SAMPLING_METHOD, util::parseNum(value));
    }
    else if(strcmp(option,"AUD.FILTER_TYPE") == 0)
    {
      wrapper->emu->set(Opt::AUD_FILTER_TYPE, util::parseNum(value));
    }
    else if(strcmp(option,"AUD.BUFFER_SIZE") == 0)
    {
      wrapper->emu->set(Opt::AUD_BUFFER_SIZE, util::parseNum(value));
    }
    else
    {
      wrapper->emu->set(Opt(util::parseEnum <OptEnum>(std::string(option))), util::parseBool(value));
      wrapper->emu->emu->update();
    }

    if(was_powered_on && must_be_off)
    {
        wrapper->emu->powerOn();
        if(was_running) wrapper->emu->run();
    }
  }
  catch(std::exception &exception) {
//    ErrorCode ec=exception.data;
//    sprintf(config_result,"%s", ErrorCodeEnum::key(ec));
    printf("unknown key wasm_configure %s = %s\n", option, value.c_str());

    sprintf(config_result,"%s", exception.what());
  }
  return config_result; 
}
extern "C" void wasm_print_error(unsigned exception_ptr)
{
  if(exception_ptr!=0)
  {
    string s= std::string(reinterpret_cast<std::exception *>(exception_ptr)->what());
    printf("uncaught exception %u: %s\n",exception_ptr, s.c_str());
  }
}


Buffer<float> leftChannel;
extern "C" u32 wasm_leftChannelBuffer()
{
    if (leftChannel.size == 0)
        leftChannel.init(2048, 0);
    return (u32)leftChannel.ptr;
}

Buffer<float> rightChannel;
extern "C" u32 wasm_rightChannelBuffer()
{
    if (rightChannel.size == 0)
        rightChannel.init(2048, 0);
    return (u32)rightChannel.ptr;
}
extern "C" void wasm_update_audio(int offset)
{
//    assert(offset == 0 || offset == leftChannel.size / 2);

  float *left = leftChannel.ptr + offset;
  float *right = rightChannel.ptr + offset;
  auto samples = leftChannel.size / 2;
  wrapper->emu->audioPort.copyStereo(left, right, samples);
  sum_samples += samples; 
}

extern "C" void wasm_write_string_to_ser(char* chars_to_send)
{
    if(wrapper->emu->agnus.agnus->id[SLOT_SER] != SER_RECEIVE)
    {
      wrapper->emu->remoteManager.remoteManager->serServer.didConnect();
    }
    wrapper->emu->remoteManager.remoteManager->serServer.doProcess(chars_to_send);
}

/*extern "C" void wasm_write_bytes_to_ser(u8 *bytes_to_send, u32 length)
{
    if(wrapper->emu->agnus.id[SLOT_SER] != SER_RECEIVE)
    {
      wrapper->emu->remoteManager.serServer.didConnect();
    }
    auto &serserver = wrapper->emu->remoteManager.serServer;
    for (u32 i=0; i<length; i++) { 
      serserver.processIncomingByte((u8)bytes_to_send[i]); 
    }
}*/

extern "C" void wasm_write_byte_to_ser(u8 byte_to_send)
{
    if(wrapper->emu->agnus.agnus->id[SLOT_SER] != SER_RECEIVE)
    {
      wrapper->emu->remoteManager.remoteManager->serServer.didConnect();
    }
    wrapper->emu->remoteManager.remoteManager->serServer.processIncomingByte(byte_to_send);
}

extern "C" double wasm_activity(u8 id, u8 read_or_write)
{
    double value=0.0;
    auto dma = wrapper->emu->agnus.getStats();
    if(id==0)
      value= dma.copperActivity /(313 *120);
    else if(id==1)
      value= dma.blitterActivity /(313 *120);
    else if(id==2)
      value= dma.diskActivity /(313 *3);
    else if(id==3)
      value= dma.audioActivity /(313 *4);
    else if(id==4)
      value= dma.spriteActivity /(313 *16);
    else if(id==5)
      value= dma.bitplaneActivity /(39330);
    else if(id==6)
    {
      /* let mem = amiga.mem.getStats()
        let max = Float((HPOS_CNT_PAL * VPOS_CNT) / 2)
        let chipR = Float(mem.chipReads.accumulated) / max
        let chipW = Float(mem.chipWrites.accumulated) / max
        let slowR = Float(mem.slowReads.accumulated) / max
        let slowW = Float(mem.slowWrites.accumulated) / max
        let fastR = Float(mem.fastReads.accumulated) / max
        let fastW = Float(mem.fastWrites.accumulated) / max
        let kickR = Float(mem.kickReads.accumulated) / max
        let kickW = Float(mem.kickWrites.accumulated) / max
        
        addValues(Monitors.Monitor.chipRam, chipR, chipW)
        addValues(Monitors.Monitor.slowRam, slowR, slowW)
        addValues(Monitors.Monitor.fastRam, fastR, fastW)
        addValues(Monitors.Monitor.kickRom, kickR, kickW)*/
        auto mem = wrapper->emu->mem.getStats();
        auto max = float(PAL::HPOS_CNT * VPOS_CNT) / 2.0;
        value = float(
          read_or_write == 0 ? mem.chipReads.accumulated : mem.chipWrites.accumulated
          )  / max;
    }
    else if(id==7)
    {
        auto mem = wrapper->emu->mem.getStats();
        auto max = float(PAL::HPOS_CNT * VPOS_CNT) / 2.0;
        value= float(
          read_or_write == 0 ? mem.slowReads.accumulated : mem.slowWrites.accumulated
          )  / max;
    }
    else if(id==8)
    {
        auto mem = wrapper->emu->mem.getStats();
        auto max = float(PAL::HPOS_CNT * VPOS_CNT) / 2.0;
        value = float(
          read_or_write == 0 ? mem.fastReads.accumulated : mem.fastWrites.accumulated
          )  / max;
    }
    else if(id==9)
    {
        auto mem = wrapper->emu->mem.getStats();
        auto max = float(PAL::HPOS_CNT * VPOS_CNT) / 2.0;
        value = float(
          read_or_write == 0 ? mem.kickReads.accumulated : mem.kickWrites.accumulated
          )  / max;
    }

  //  printf("activity_id: %u, rw: %u =%lf\n",id, read_or_write,value);

    return value;
}


Thumbnail preview;
extern "C" char* wasm_save_workspace(char* path)
{
  try{
    //save with DMA_DEBUG_ENABLE=false otherwise vAmiga.app for macOS would trigger minimized debug screen 
    auto debug_enable = wrapper->emu->get(Opt::DMA_DEBUG_ENABLE);
    wrapper->emu->set(Opt::DMA_DEBUG_ENABLE,false);
    wrapper->emu->emu->update();

    wrapper->emu->amiga.saveWorkspace(path);

    wrapper->emu->set(Opt::DMA_DEBUG_ENABLE,debug_enable);
    wrapper->emu->emu->update();
  }
  catch (const AppError& e) {
    printf("Error %s\n", e.what());
    EM_ASM(
    {
      alert(`Error - ${UTF8ToString($0)}`);
    }, e.what());    
  }

  preview.take(*(wrapper->emu->amiga.amiga));

  sprintf(wasm_pull_user_snapshot_file_json_result, "{\"address\":%lu, \"size\": %u, \"width\": %d, \"height\":%d }",
    (unsigned long)preview.screen, 
    preview.width*preview.height*4,
    preview.width,
    preview.height
    );
  return wasm_pull_user_snapshot_file_json_result;
}

extern "C" void wasm_load_workspace(char* path)
{
  try{
    //don't respect the DMA_DEBUG_ENABLE setting in the workspace file
    //instead keep current user choice
    auto debug_enable = wrapper->emu->get(Opt::DMA_DEBUG_ENABLE);
    auto channel0 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL0);
    auto channel1 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL1);
    auto channel2 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL2);
    auto channel3 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL3);
    auto channel4 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL4);
    auto channel5 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL5);
    auto channel6 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL6);
    auto channel7 = wrapper->emu->get(Opt::DMA_DEBUG_CHANNEL7);

    wrapper->emu->amiga.loadWorkspace(path);

    wrapper->emu->set(Opt::DMA_DEBUG_ENABLE,debug_enable);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL0,channel0);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL1,channel1);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL2,channel2);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL3,channel3);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL4,channel4);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL5,channel5);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL6,channel6);
    wrapper->emu->set(Opt::DMA_DEBUG_CHANNEL7,channel7);
    wrapper->emu->emu->update();

  }
  catch (const AppError& e) {
    printf("Error %s\n", e.what());
    EM_ASM(
    {
      alert(`Error - ${UTF8ToString($0)}`);
    }, e.what());    
  }
}

extern "C" void wasm_retro_shell(char* cmd)
{
  if( strcmp(cmd,"unmute") == 0)
  {
    printf("do unmute\n");
    wrapper->emu->audioPort.port->unmute(10000);
  }
  else
    wrapper->emu->retroShell.execScript(cmd);
}


////////////////////////////////////////
// GB additions
////////////////////////////////////////


// Debugger functions:

// main.cpu.debugger.stepOver();
extern "C" void wasm_step_over() { wrapper->emu->stepOver(); }

extern "C" void wasm_step_into() { wrapper->emu->stepInto(); }

// Time-travel replay: result registers for returning 64-bit values as lo/hi u32 pairs
static u32 g_instr_count_lo = 0, g_instr_count_hi = 0;
static u32 g_replay_scan_lo = 0, g_replay_scan_hi = 0;
static constexpr uint64_t REPLAY_NO_MATCH = UINT64_MAX;

extern "C" void wasm_read_instr_count() {
    uint64_t count = wrapper->emu->cpu.cpu->instrCount;
    g_instr_count_lo = (u32)(count & 0xFFFFFFFF);
    g_instr_count_hi = (u32)(count >> 32);
}
extern "C" u32 wasm_get_instr_count_lo() { return g_instr_count_lo; }
extern "C" u32 wasm_get_instr_count_hi() { return g_instr_count_hi; }

extern "C" u32 wasm_get_replay_scan_lo() { return g_replay_scan_lo; }
extern "C" u32 wasm_get_replay_scan_hi() { return g_replay_scan_hi; }

// Replay N instructions synchronously (full Amiga: CPU + DMA via Moira::sync).
// Breakpoints are suppressed so they don't fire during replay.
extern "C" void wasm_replay_instructions_video(u32 count) {
    auto *cpu = wrapper->emu->cpu.cpu;
    bool hadBP = cpu->debugger.breakpoints.elements() != 0;
    cpu->debugger.breakpoints.setNeedsCheck(false);

    for (u32 i = 0; i < count; i++) {
        cpu->execute();
    }

    if (hadBP) cpu->debugger.breakpoints.setNeedsCheck(true);
}

// Scan N instructions for a breakpoint PC match.
// Returns the instrCount of the LATEST match (or REPLAY_NO_MATCH) in g_replay_scan_lo/hi.
extern "C" void wasm_replay_scan(u32 count) {
    auto *cpu = wrapper->emu->cpu.cpu;
    bool hadBP = cpu->debugger.breakpoints.elements() != 0;
    cpu->debugger.breakpoints.setNeedsCheck(false);

    uint64_t match = REPLAY_NO_MATCH;
    for (u32 i = 0; i < count; i++) {
        cpu->execute();
        if (cpu->breakpoints.isSetAt(cpu->getPC0())) {
            match = cpu->instrCount;
        }
    }

    if (hadBP) cpu->debugger.breakpoints.setNeedsCheck(true);
    g_replay_scan_lo = (u32)(match & 0xFFFFFFFF);
    g_replay_scan_hi = (u32)(match >> 32);
}

// Scan N instructions for a frame boundary (vblank entry transition).
// Returns the instrCount of the LATEST frame boundary in g_replay_scan_lo/hi.
extern "C" void wasm_replay_scan_frame(u32 count) {
    auto *cpu = wrapper->emu->cpu.cpu;
    auto *agnus = wrapper->emu->agnus.agnus;
    bool hadBP = cpu->debugger.breakpoints.elements() != 0;
    cpu->debugger.breakpoints.setNeedsCheck(false);

    uint64_t match = REPLAY_NO_MATCH;
    for (u32 i = 0; i < count; i++) {
        bool wasVBlank = agnus->inVBlankArea();
        cpu->execute();
        if (!wasVBlank && agnus->inVBlankArea()) {
            match = cpu->instrCount;
        }
    }

    if (hadBP) cpu->debugger.breakpoints.setNeedsCheck(true);
    g_replay_scan_lo = (u32)(match & 0xFFFFFFFF);
    g_replay_scan_hi = (u32)(match >> 32);
}

// Breakpoints:

extern "C" const char *wasm_list_breakpoints() {
  static char buffer[4096];
  try {
    auto &bp = wrapper->emu->cpu.breakpoints;
    snprintf(buffer, sizeof(buffer), "{\"breakpoints\":[");

    for (int i = 0; i < bp.elements(); i++) {
      if (i > 0)
        strcat(buffer, ",");
      auto guard = bp.guardNr(i);
      if (guard) {
        char entry[256];
        snprintf(
            entry, sizeof(entry),
            "{\"nr\":%d,\"addr\":\"0x%08X\",\"enabled\":%s,\"ignore\":%ld}", i,
            guard->addr, guard->enabled ? "true" : "false", guard->ignore);
        strcat(buffer, entry);
      }
    }
    strcat(buffer, "]}");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" bool wasm_set_breakpoint(u32 addr, u32 ignores = 0) {
  try {
    wrapper->emu->cpu.breakpoints.setAt(addr, ignores);
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_remove_breakpoint(u32 addr) {
  try {
    wrapper->emu->cpu.breakpoints.removeAt(addr);
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_remove_all_breakpoints() {
  try {
    wrapper->emu->cpu.breakpoints.removeAll();
    return true;
  } catch(...) {
    return false;
  }
}

// Watchpoints:

extern "C" const char *wasm_list_watchpoints() {
  static char buffer[4096];
  try {
    auto &wp = wrapper->emu->cpu.watchpoints;
    snprintf(buffer, sizeof(buffer), "{\"watchpoints\":[");

    for (int i = 0; i < wp.elements(); i++) {
      if (i > 0)
        strcat(buffer, ",");
      auto guard = wp.guardNr(i);
      if (guard) {
        char entry[256];
        snprintf(
            entry, sizeof(entry),
            "{\"nr\":%d,\"addr\":\"0x%08X\",\"enabled\":%s,\"ignore\":%ld}", i,
            guard->addr, guard->enabled ? "true" : "false", guard->ignore);
        strcat(buffer, entry);
      }
    }
    strcat(buffer, "]}");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" bool wasm_set_watchpoint(u32 addr, u32 ignores = 0) {
  try {
    wrapper->emu->cpu.watchpoints.setAt(addr, ignores);
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_remove_watchpoint(u32 addr) {
  try {
    wrapper->emu->cpu.watchpoints.removeAt(addr);
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_remove_all_watchpoints() {
  try {
    wrapper->emu->cpu.watchpoints.removeAll();
    return true;
  } catch(...) {
    return false;
  }
}

// Catchpoints:

extern "C" const char *wasm_list_catchpoints() {
  static char buffer[4096];
  try {
    auto &cp = wrapper->emu->cpu.cpu->catchpoints;
    snprintf(buffer, sizeof(buffer), "{\"catchpoints\":[");

    for (int i = 0; i < cp.elements(); i++) {
      if (i > 0)
        strcat(buffer, ",");
      auto guard = cp.guardNr(i);
      if (guard) {
        char entry[256];
        snprintf(entry, sizeof(entry),
                  "{\"nr\":%d,\"vector\":%d,\"enabled\":%s,\"ignore\":%ld}", i,
                  guard->addr, guard->enabled ? "true" : "false",
                  guard->ignore);
        strcat(buffer, entry);
      }
    }
    strcat(buffer, "]}");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" bool wasm_remove_all_catchpoints() {
  try {
    wrapper->emu->cpu.cpu->catchpoints.removeAll();
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_set_catchpoint(u32 vector, u32 ignores = 0) {
  try {
    wrapper->emu->cpu.cpu->catchpoints.setAt(vector, ignores);
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_remove_catchpoint(u32 vector) {
  try {
    wrapper->emu->cpu.cpu->catchpoints.removeAt(vector);
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_eol() {
  try {
    wrapper->emu->finishLine();
    return true;
  } catch(...) {
    return false;
  }
}

extern "C" bool wasm_eof() {
  try {
    wrapper->emu->finishFrame();
    return true;
  } catch(...) {
    return false;
  }
}


// Data read/write funcitons

extern "C" const char *wasm_debug_emulator_state() {
  static char result_buffer[512];

  try {
    auto *emu = wrapper->emu;

    snprintf(result_buffer, sizeof(result_buffer),
      "{\"isRunning\":%s,\"isPaused\":%s,\"isPoweredOn\":%s}",
      emu->isRunning() ? "true" : "false",
      emu->isPaused() ? "true" : "false",
      emu->isPoweredOn() ? "true" : "false"
    );
    return result_buffer;

  } catch (const std::exception& e) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":\"Exception: %s\"}", e.what());
    return result_buffer;
  } catch (...) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":\"Unknown error occurred\"}");
    return result_buffer;
  }
}

extern "C" const char *wasm_set_register(const char* reg_name, u32 value) {
  static char result_buffer[256];

  // Helper functions to reduce repetition
  auto success32 = [&](u32 val) -> const char* {
    snprintf(result_buffer, sizeof(result_buffer), "{\"success\":true,\"register\":\"%s\",\"value\":\"0x%08X\"}", reg_name, val);
    return result_buffer;
  };
  auto success16 = [&](u16 val) -> const char* {
    snprintf(result_buffer, sizeof(result_buffer), "{\"success\":true,\"register\":\"%s\",\"value\":\"0x%04X\"}", reg_name, val);
    return result_buffer;
  };
  auto success8 = [&](u8 val) -> const char* {
    snprintf(result_buffer, sizeof(result_buffer), "{\"success\":true,\"register\":\"%s\",\"value\":\"0x%02X\"}", reg_name, val);
    return result_buffer;
  };
  auto error = [&](const char* msg) -> const char* {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":true,\"message\":\"%s\",\"register\":\"%s\"}", msg, reg_name);
    return result_buffer;
  };

  try {
    std::string regName(reg_name);
    auto *cpu = wrapper->emu->cpu.cpu;

    // Data registers (d0-d7)
    if (regName.length() == 2 && regName[0] == 'd' && regName[1] >= '0' && regName[1] <= '7') {
      cpu->setD(regName[1] - '0', value);
      return success32(value);
    }
    // Address registers (a0-a7)
    if (regName.length() == 2 && regName[0] == 'a' && regName[1] >= '0' && regName[1] <= '7') {
      cpu->setA(regName[1] - '0', value);
      return success32(value);
    }
    if (regName == "pc") {
      cpu->setPC(value);
      return success32(value);
    }
    if (regName == "sr") {
      cpu->setSR(value & 0xFFFF);
      return success16(value & 0xFFFF);
    }
    if (regName == "usp") {
      cpu->setUSP(value);
      return success32(value);
    }
    if (regName == "msp") {
      cpu->setMSP(value);
      return success32(value);
    }
    if (regName == "isp") {
      cpu->setISP(value);
      return success32(value);
    }

    if (regName == "vbr") {
      cpu->setVBR(value);
      return success32(value);
    }
    if (regName == "sfc") {
      cpu->setSFC(value);
      return success8(value);
    }
    if (regName == "dfc") {
      cpu->setDFC(value);
      return success8(value);
    }
    if (regName == "caar") {
      cpu->setCAAR(value);
      return success8(value);
    }
    if (regName == "cacr") {
      cpu->setCACR(value);
      return success8(value);
    }
    if (regName == "irc") {
      cpu->setIRC(value & 0xFFFF);
      return success16(value);
    }

    return error("Unsupported register name");

  } catch (const std::exception& e) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":true,\"message\":\"Exception: %s\"}", e.what());
    return result_buffer;
  } catch (...) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":true,\"message\":\"Unknown error occurred\"}");
    return result_buffer;
  }
}

extern "C" u8 wasm_peek8(u32 addr) {
  return wrapper->emu->mem.mem->spypeek8<Accessor::CPU>(addr);
}

extern "C" u16 wasm_peek16(u32 addr) {
  return wrapper->emu->mem.mem->spypeek16<Accessor::CPU>(addr);
}

extern "C" u32 wasm_peek32(u32 addr) {
  u16 hi = wrapper->emu->mem.mem->spypeek16<Accessor::CPU>(addr);
  u16 lo = wrapper->emu->mem.mem->spypeek16<Accessor::CPU>(addr + 2);
  return (hi << 16) | lo;
}

extern "C" u16 wasm_peek_custom(u32 addr) {
  return wrapper->emu->mem.mem->spypeekCustom16(addr);
}

extern "C" void wasm_poke8(u32 addr, u8 value) {
  wrapper->emu->mem.mem->poke8<Accessor::CPU>(addr, value);
}

extern "C" void wasm_poke16(u32 addr, u16 value) {
  wrapper->emu->mem.mem->poke16<Accessor::CPU>(addr, value);
}

extern "C" void wasm_poke32(u32 addr, u32 value) {
  wrapper->emu->mem.mem->poke16<Accessor::CPU>(addr, value >> 16);
  wrapper->emu->mem.mem->poke16<Accessor::CPU>(addr + 2, value & 0xffff);
}

extern "C" void wasm_poke_custom16(u32 addr, u16 value) {
  wrapper->emu->mem.mem->pokeCustom16<Accessor::CPU>(addr, value);
}

extern "C" void wasm_poke_custom32(u32 addr, u32 value) {
  wrapper->emu->mem.mem->pokeCustom16<Accessor::CPU>(addr, value >> 16);
  wrapper->emu->mem.mem->pokeCustom16<Accessor::CPU>(addr + 2, value & 0xffff);
}


// CPU Tracing:

extern "C" bool wasm_enable_cpu_logging(bool enable) {
  try {
    if (enable) {
      wrapper->emu->cpu.cpu->debugger.enableLogging();
    } else {
      wrapper->emu->cpu.cpu->debugger.disableLogging();
    }
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" void wasm_clear_cpu_trace() {
  try {
    wrapper->emu->cpu.cpu->debugger.clearLog();
  } catch (...) {
    // Ignore errors
  }
}

extern "C" u8* wasm_read_memory(u32 address, u32 count) {
  try {
    if (count == 0) {
      return nullptr;
    }

    u8* buffer = (u8*)malloc(count);
    if (!buffer) {
      return nullptr;
    }

    for (u32 i = 0; i < count; i++) {
      buffer[i] = wrapper->emu->mem.mem->spypeek8<Accessor::CPU>(address + i);
    }

    return buffer;

  } catch (...) {
    return nullptr;
  }
}

extern "C" bool wasm_write_memory(u32 address, u8* data, u32 count) {
  try {
    if (!data || count == 0) {
      return false;
    }

    for (u32 i = 0; i < count; i++) {
      wrapper->emu->mem.mem->poke8<Accessor::CPU>(address + i, data[i]);
    }

    return true;

  } catch (...) {
    return false;
  }
}

// [vscode-vamiga-debugger cpu profiler] CPU profiler control + readout.
// Logic lives in Core/Profiler/CpuProfiler.{h,cpp}; these are thin wasm wrappers.

// Per-capture timing measured around the profiled frame, so the host can convert
// cycles to time without assuming a clock (handles CPU revision/overclock/boost and
// PAL/NTSC): the actual CPU-clock cycles in one frame, and the video standard.
static i64 gProfileFrameCycles = 0;
static bool gProfileIsPAL = true;

// Upload the per-code-location unwind table (one {cfa,r13,ra} entry per 2 bytes,
// built by the extension from DWARF .debug_frame) and the program's text range.
extern "C" bool wasm_profile_set_unwind(u8* data, u32 len, u32 startAddr, u32 endAddr) {
  try {
    CpuProfiler::setMemory(wrapper->emu->mem.mem);
    CpuProfiler::setUnwind(data, len, startAddr, endAddr);
    return true;
  } catch (...) {
    return false;
  }
}

// Capture numFrames frames (Phase 1: 1) synchronously: enable the profiler, run
// the frame(s) via computeFrame(), then disable. Results are read with get_data().
extern "C" bool wasm_profile_start(u32 numFrames) {
  try {
    if (numFrames == 0) numFrames = 1;

    // Use computeFrame() (the synchronous run-loop primitive that emulates one full
    // frame right now), NOT finishFrame(): finishFrame() merely ARMS an end-of-frame
    // trap for the async render loop, so it executes 0 instructions synchronously and
    // also halts the emulator when the trap later fires. computeFrame() runs the CPU
    // immediately and leaves the run state untouched, so execution continues after.
    //
    // First call finishes the current partial frame (alignment), so each profiled
    // frame below is captured whole from its first instruction.
    wrapper->emu->emu->computeFrame();

    CpuProfiler::start();
    wrapper->emu->cpu.cpu->enableProfiling();
    // [vscode-vamiga-debugger dma profiler] Capture DMA in the SAME measured frame so
    // the DMA line shares the CPU flame's timeline. start() snapshots chip/slow RAM at
    // this frame boundary; the per-line/per-cycle hooks fire during computeFrame().
    DmaProfiler::setMemory(wrapper->emu->mem.mem);
    DmaProfiler::start();
    // Bracket the profiled frame(s) with the CPU clock to measure cycles/frame.
    const i64 clockBefore = wrapper->emu->cpu.cpu->getClock();
    for (u32 i = 0; i < numFrames; i++) wrapper->emu->emu->computeFrame();
    gProfileFrameCycles = (wrapper->emu->cpu.cpu->getClock() - clockBefore) / (i64)numFrames;
    gProfileIsPAL = wrapper->emu->agnus.agnus->isPAL();
    wrapper->emu->cpu.cpu->disableProfiling();
    CpuProfiler::stop();
    DmaProfiler::stop();
    return true;
  } catch (...) {
    wrapper->emu->cpu.cpu->disableProfiling();
    CpuProfiler::stop();
    DmaProfiler::stop();
    printf("[cpu-profiler] wasm_profile_start: EXCEPTION\n");
    return false;
  }
}

// Safety stop (idempotent) — capture is normally self-contained in start().
extern "C" void wasm_profile_stop() {
  wrapper->emu->cpu.cpu->disableProfiling();
  CpuProfiler::stop();
}

// Return {address,size} of the raw u32 profile stream for the JS side to read off
// HEAPU8 (same pattern as snapshots). Valid until the next wasm_profile_start().
extern "C" const char* wasm_profile_get_data() {
  static char result_buffer[256];
  const u32* data = CpuProfiler::data();
  u32 words = CpuProfiler::count();
  // Include capture diagnostics so the host can explain an empty result.
  sprintf(result_buffer,
    "{\"address\":%lu, \"size\":%lu, \"start\":%lu, \"end\":%lu, \"total\":%lu, \"inRange\":%lu, "
    "\"frameCycles\":%lld, \"isPAL\":%s}",
    (unsigned long)data, (unsigned long)(words * 4),
    (unsigned long)CpuProfiler::rangeStart(), (unsigned long)CpuProfiler::rangeEnd(),
    (unsigned long)CpuProfiler::totalInstr(), (unsigned long)CpuProfiler::inRangeInstr(),
    (long long)gProfileFrameCycles, gProfileIsPAL ? "true" : "false");
  return result_buffer;
}

// [vscode-vamiga-debugger dma profiler] Return {address,size} of the enriched DMA grid
// (Cell[8]) for the JS side to read off HEAPU8. Captured in the same frame as the CPU
// profile (wasm_profile_start); valid until the next wasm_profile_start().
extern "C" const char* wasm_dma_get_data() {
  static char result_buffer[128];
  sprintf(result_buffer, "{\"address\":%lu, \"size\":%lu}",
    (unsigned long)DmaProfiler::gridData(), (unsigned long)DmaProfiler::gridLen());
  return result_buffer;
}

// [vscode-vamiga-debugger dma profiler] Return the reconstruction baseline snapshot
// (chip + slow RAM) taken at capture start. Custom-register baseline is deferred
// (customLen 0). Buffers are valid until the next wasm_profile_start().
extern "C" const char* wasm_dma_get_snapshot() {
  static char result_buffer[256];
  sprintf(result_buffer,
    "{\"chipAddr\":%lu, \"chipLen\":%lu, \"slowAddr\":%lu, \"slowLen\":%lu, "
    "\"customAddr\":0, \"customLen\":0}",
    (unsigned long)DmaProfiler::chipData(), (unsigned long)DmaProfiler::chipLen(),
    (unsigned long)DmaProfiler::slowData(), (unsigned long)DmaProfiler::slowLen());
  return result_buffer;
}

extern "C" const char* wasm_jump(u32 address) {
  static char result_buffer[256];

  try {
    wrapper->emu->cpu.cpu->jump(address);

    snprintf(result_buffer, sizeof(result_buffer),
      "{\"success\":true,\"address\":\"0x%08X\"}", address);
    return result_buffer;

  } catch (const std::exception& e) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":\"Exception: %s\"}", e.what());
    return result_buffer;
  } catch (...) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":\"Unknown error occurred\"}");
    return result_buffer;
  }
}

extern "C" const char *wasm_get_cpu_trace(u32 count) {
  static std::string result_buffer;

  try {
    using namespace vamiga;

    // Get number of logged instructions
    isize logged = wrapper->emu->cpu.cpu->debugger.loggedInstructions();

    if (logged == 0) {
      result_buffer = "{\"error\":\"No trace data available. CPU logging "
                      "might be disabled.\"}";
      return result_buffer.c_str();
    }

    // Limit count to available instructions
    if (count > logged)
      count = logged;

    result_buffer = "{";
    result_buffer += "\"total_logged\":" + std::to_string(logged) + ",";
    result_buffer += "\"returned\":" + std::to_string(count) + ",";
    result_buffer += "\"trace\":[";

    // Get the most recent 'count' instructions (working backwards)
    for (u32 i = 0; i < count; i++) {
      if (i > 0)
        result_buffer += ",";

      // logEntryAbs uses absolute indexing - 0 = oldest, logged-1 = newest
      isize trace_index = logged - count + i;
      isize instr_len = 0;
      const char *pc_str =
          wrapper->emu->cpu.cpu->disassembleRecordedPC(trace_index);
      const char *instr_str = wrapper->emu->cpu.cpu->disassembleRecordedInstr(
          trace_index, &instr_len);
      const char *flags_str =
          wrapper->emu->cpu.cpu->disassembleRecordedFlags(trace_index);

      result_buffer += "{";
      result_buffer += "\"pc\":\"" + std::string(pc_str) + "\",";
      result_buffer += "\"instruction\":\"" + std::string(instr_str) + "\",";
      result_buffer += "\"flags\":\"" + std::string(flags_str) + "\",";
      result_buffer += "\"length\":" + std::to_string(instr_len);
      result_buffer += "}";
    }

    result_buffer += "]}";

  } catch (...) {
    result_buffer = "{\"error\":\"Failed to get CPU trace\"}";
  }

  return result_buffer.c_str();
}

// Memory Debugging Functions:

extern "C" const char *wasm_disassemble(u32 addr, u32 count) {
  static std::string result_buffer;

  try {
    using namespace vamiga;

    result_buffer = "{";
    result_buffer += "\"start_addr\":\"";

    char addr_hex[16];
    sprintf(addr_hex, "0x%08X", addr);
    result_buffer += std::string(addr_hex) + "\",";
    result_buffer += "\"count\":" + std::to_string(count) + ",";
    result_buffer += "\"instructions\":[";

    u32 current_addr = addr;

    for (u32 i = 0; i < count; i++) {
      if (i > 0)
        result_buffer += ",";

      isize instr_len = 0;
      const char *instruction =
          wrapper->emu->cpu.cpu->disassembleInstr(current_addr, &instr_len);
      const char *hex_words = wrapper->emu->cpu.cpu->disassembleWords(
          current_addr, instr_len / 2);
      const char *addr_str =
          wrapper->emu->cpu.cpu->disassembleAddr(current_addr);

      result_buffer += "{";
      result_buffer += "\"addr\":\"" + std::string(addr_str) + "\",";
      result_buffer +=
          "\"instruction\":\"" + std::string(instruction) + "\",";
      result_buffer += "\"hex\":\"" + std::string(hex_words) + "\",";
      result_buffer += "\"length\":" + std::to_string(instr_len);
      result_buffer += "}";

      current_addr += instr_len;
    }

    result_buffer += "]}";

  } catch (...) {
    result_buffer = "{\"error\":\"Failed to disassemble memory\"}";
  }

  return result_buffer.c_str();
}

extern "C" const char *wasm_disassemble_copper(u32 addr, u32 count) {
  static std::string result_buffer;

  try {
    using namespace vamiga;

    result_buffer = "{";
    result_buffer += "\"start_addr\":\"";

    char addr_hex[16];
    sprintf(addr_hex, "0x%08X", addr);
    result_buffer += std::string(addr_hex) + "\",";
    result_buffer += "\"count\":" + std::to_string(count) + ",";
    result_buffer += "\"instructions\":[";

    u32 current_addr = addr;

    for (u32 i = 0; i < count; i++) {
      if (i > 0)
        result_buffer += ",";

      // Each copper instruction is 4 bytes (2 words)
      std::string instruction = wrapper->emu->agnus.copper.disassemble(current_addr, true);

      // Get the hex words for the copper instruction (2 words)
      u16 word1 = wrapper->emu->mem.mem->spypeek16<Accessor::CPU>(current_addr);
      u16 word2 = wrapper->emu->mem.mem->spypeek16<Accessor::CPU>(current_addr + 2);

      char hex_words[16];
      sprintf(hex_words, "%04X %04X", word1, word2);

      char addr_str[16];
      sprintf(addr_str, "%06X", current_addr);

      result_buffer += "{";
      result_buffer += "\"addr\":\"" + std::string(addr_str) + "\",";
      result_buffer += "\"instruction\":\"" + instruction + "\",";
      result_buffer += "\"hex\":\"" + std::string(hex_words) + "\",";
      result_buffer += "\"length\":4";
      result_buffer += "}";

      current_addr += 4; // Copper instructions are always 4 bytes
    }

    result_buffer += "]}";

  } catch (...) {
    result_buffer = "{\"error\":\"Failed to disassemble copper instructions\"}";
  }

  return result_buffer.c_str();
}

extern "C" const char *wasm_hex_dump(u32 addr, u32 bytes) {
  static char buffer[8192];
  try {
    auto result =
        wrapper->emu->mem.debugger.hexDump(Accessor::CPU, addr, bytes, 1);
    strncpy(buffer, result.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
  } catch (...) {
    return "error";
  }
}

extern "C" const char *wasm_mem_dump(u32 addr, u32 bytes) {
  static char buffer[8192];
  try {
    auto result =
        wrapper->emu->mem.debugger.memDump(Accessor::CPU, addr, bytes, 1);
    strncpy(buffer, result.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
  } catch (...) {
    return "error";
  }
}

extern "C" const char *wasm_asc_dump(u32 addr, u32 bytes) {
  static char buffer[4096];
  try {
    auto result =
        wrapper->emu->mem.debugger.ascDump(Accessor::CPU, addr, bytes);
    strncpy(buffer, result.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
  } catch (...) {
    return "error";
  }
}


// Component Info Functions

extern "C" const char *wasm_get_amiga_info() {
  static char buffer[1024];
  try {
    const auto &info = wrapper->emu->amiga.getInfo();
    snprintf(buffer, sizeof(buffer),
              "{\"cpuClock\":%lld,\"dmaClock\":%lld,\"ciaAClock\":%lld,"
              "\"ciaBClock\":%lld,\"frame\":%lld,\"vpos\":%ld,\"hpos\":%ld}",
              (long long)info.cpuClock, (long long)info.dmaClock,
              (long long)info.ciaAClock, (long long)info.ciaBClock,
              (long long)info.frame, info.vpos, info.hpos);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_cpu_info() {
  static char buffer[1024];
  try {
    const auto &info = wrapper->emu->cpu.getInfo();

    // - usp - User Stack Pointer
    // - isp - Interrupt Stack Pointer
    // - msp - Master Stack Pointer (68020+)
    // - vbr - Vector Base Register (68010+)
    // - irc - Instruction Register Cache
    // - sfc/dfc - Source/Destination Function Code (68010+)
    // - cacr/caar - Cache Control/Address (68020+)
    snprintf(buffer, sizeof(buffer),
              "{\"pc\":\"0x%08X\",\"d0\":\"0x%08X\",\"d1\":"
              "\"0x%08X\",\"d2\":\"0x%08X\",\"d3\":\"0x%08X\",\"d4\":\"0x%"
              "08X\",\"d5\":\"0x%08X\",\"d6\":\"0x%08X\",\"d7\":\"0x%08X\","
              "\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":"
              "\"0x%08X\",\"a4\":\"0x%08X\",\"a5\":\"0x%08X\",\"a6\":\"0x%"
              "08X\",\"a7\":\"0x%08X\",\"sr\":\"0x%04X\",\"usp\":\"0x%08X\","
              "\"isp\":\"0x%08X\",\"msp\":\"0x%08X\",\"vbr\":\"0x%08X\","
              "\"irc\":\"0x%04X\",\"sfc\":\"0x%02X\",\"dfc\":\"0x%02X\","
              "\"cacr\":\"0x%02X\",\"caar\":\"0x%02X\"}",
              info.pc0, info.d[0], info.d[1], info.d[2],
              info.d[3], info.d[4], info.d[5], info.d[6], info.d[7], info.a[0],
              info.a[1], info.a[2], info.a[3], info.a[4], info.a[5], info.a[6],
              info.a[7], info.sr, info.usp, info.isp, info.msp, info.vbr,
              info.irc, info.sfc, info.dfc, info.cacr, info.caar);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_agnus_info() {
  static char buffer[1024];
  try {
    const auto &info = wrapper->emu->agnus.getInfo();
    snprintf(buffer, sizeof(buffer),
              "{\"vpos\":%d,\"hpos\":%d,\"frame\":%lld,\"dmacon\":\"0x%04X\","
              "\"bplcon0\":\"0x%04X\",\"ddfstrt\":\"0x%04X\",\"ddfstop\":\"0x%"
              "04X\",\"diwstrt\":\"0x%04X\",\"diwstop\":\"0x%04X\","
              "\"bpl1mod\":\"0x%04X\",\"bpl2mod\":\"0x%04X\"}",
              (int)info.vpos, (int)info.hpos, (long long)info.frame,
              info.dmacon, info.bplcon0, info.ddfstrt, info.ddfstop,
              info.diwstrt, info.diwstop, info.bpl1mod, info.bpl2mod);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_paula_info() {
  static char buffer[1024];
  try {
    const auto &info = wrapper->emu->paula.getInfo();
    snprintf(
        buffer, sizeof(buffer),
        "{\"intreq\":\"0x%04X\",\"intena\":\"0x%04X\",\"adkcon\":\"0x%04X\"}",
        info.intreq, info.intena, info.adkcon);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_denise_info() {
  static char buffer[1024];
  try {
    const auto &info = wrapper->emu->denise.getInfo();
    snprintf(buffer, sizeof(buffer),
              "{\"ecs\":%s,\"bplcon0\":\"0x%04X\",\"bplcon1\":\"0x%04X\","
              "\"bplcon2\":\"0x%04X\",\"bpu\":%d,\"joydat0\":\"0x%04X\","
              "\"joydat1\":\"0x%04X\",\"clxdat\":\"0x%04X\",\"diwstrt\":\"0x%"
              "04X\",\"diwstop\":\"0x%04X\"}",
              info.ecs ? "true" : "false", info.bplcon0, info.bplcon1,
              info.bplcon2, info.bpu, info.joydat[0], info.joydat[1],
              info.clxdat, info.diwstrt, info.diwstop);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_memory_info() {
  static char buffer[4096];
  try {
    const auto &info = wrapper->emu->mem.getInfo();

    // Build JSON with basic memory info first
    // Mask values are (memorySize - 1) and used for address wrapping within memory regions
    // e.g. chipMask=0x7FFFF for 512KB chip RAM, romMask=0x7FFFF for 512KB ROM
    std::string json = "{";
    char basicBuf[512];
    snprintf(basicBuf, sizeof(basicBuf),
        "\"hasRom\":%s,\"hasWom\":%s,\"hasExt\":%s,\"hasBootRom\":%s,"
        "\"hasKickRom\":%s,\"womLock\":%s,\"romMask\":\"0x%08X\",\"womMask\":"
        "\"0x%08X\",\"extMask\":\"0x%08X\",\"chipMask\":\"0x%08X\"",
        info.hasRom ? "true" : "false", info.hasWom ? "true" : "false",
        info.hasExt ? "true" : "false", info.hasBootRom ? "true" : "false",
        info.hasKickRom ? "true" : "false", info.womLock ? "true" : "false",
        info.romMask, info.womMask, info.extMask, info.chipMask);
    json += basicBuf;

    // Add memory source arrays using numeric values
    // MemSrc enum values: 0=NONE, 1=CHIP, 2=CHIP_MIRROR, 3=SLOW, 4=SLOW_MIRROR,
    // 5=FAST, 6=CIA, 7=CIA_MIRROR, 8=RTC, 9=CUSTOM, 10=CUSTOM_MIRROR,
    // 11=AUTOCONF, 12=ZOR, 13=ROM, 14=ROM_MIRROR, 15=WOM, 16=EXT
    json += ",\"cpuMemSrc\":[";
    for (int i = 0; i < 256; i++) {
      if (i > 0) json += ",";
      json += std::to_string((long)info.cpuMemSrc[i]);
    }
    json += "],\"agnusMemSrc\":[";
    for (int i = 0; i < 256; i++) {
      if (i > 0) json += ",";
      json += std::to_string((long)info.agnusMemSrc[i]);
    }
    json += "]}";

    strncpy(buffer, json.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_audio_channel_info(int channel) {
  static char buffer[512];
  try {
    const StateMachineInfo *info = nullptr;
    switch (channel) {
    case 0:
      info = &wrapper->emu->paula.audioChannel0.getInfo();
      break;
    case 1:
      info = &wrapper->emu->paula.audioChannel1.getInfo();
      break;
    case 2:
      info = &wrapper->emu->paula.audioChannel2.getInfo();
      break;
    case 3:
      info = &wrapper->emu->paula.audioChannel3.getInfo();
      break;
    default:
      return "{\"error\":\"invalid_channel\"}";
    }
    snprintf(buffer, sizeof(buffer),
              "{\"state\":%d,\"dma\":%s,\"audlen\":%d,\"audper\":%d,"
              "\"audvol\":%d,\"auddat\":\"0x%04X\"}",
              (int)info->state, info->dma ? "true" : "false", info->audlen,
              info->audper, info->audvol, info->auddat);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_audio_port_info() {
  static char buffer[512];
  try {
    const auto &info = wrapper->emu->audioPort.getInfo();
    snprintf(buffer, sizeof(buffer), "{\"isMuted\":%s}",
              info.isMuted ? "true" : "false");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_cia_info(int cia_num) {
  static char buffer[1024];
  try {
    const CIAInfo *info = nullptr;
    if (cia_num == 0) {
      info = &wrapper->emu->ciaA.getInfo();
    } else if (cia_num == 1) {
      info = &wrapper->emu->ciaB.getInfo();
    } else {
      return "{\"error\":\"invalid_cia\"}";
    }
    snprintf(
        buffer, sizeof(buffer),
        "{\"portA\":{\"port\":\"0x%02X\",\"reg\":\"0x%02X\",\"dir\":\"0x%"
        "02X\"},\"portB\":{\"port\":\"0x%02X\",\"reg\":\"0x%02X\",\"dir\":"
        "\"0x%02X\"},\"timerA\":{\"count\":%d,\"latch\":%d,\"running\":%s},"
        "\"timerB\":{\"count\":%d,\"latch\":%d,\"running\":%s},\"icr\":\"0x%"
        "02X\",\"imr\":\"0x%02X\",\"irq\":%s}",
        info->portA.port, info->portA.reg, info->portA.dir, info->portB.port,
        info->portB.reg, info->portB.dir, info->timerA.count,
        info->timerA.latch, info->timerA.running ? "true" : "false",
        info->timerB.count, info->timerB.latch,
        info->timerB.running ? "true" : "false", info->icr, info->imr,
        info->irq ? "true" : "false");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_floppy_info(int drive_num) {
  static char buffer[1024];
  try {
    const FloppyDriveInfo *info = nullptr;
    switch (drive_num) {
    case 0:
      info = &wrapper->emu->df0.getInfo();
      break;
    case 1:
      info = &wrapper->emu->df1.getInfo();
      break;
    case 2:
      info = &wrapper->emu->df2.getInfo();
      break;
    case 3:
      info = &wrapper->emu->df3.getInfo();
      break;
    default:
      return "{\"error\":\"invalid_drive\"}";
    }
    snprintf(buffer, sizeof(buffer),
              "{\"nr\":%d,\"cylinder\":%d,\"head\":%d,\"isConnected\":%s,"
              "\"hasDisk\":%s,\"hasUnmodifiedDisk\":%s,\"hasModifiedDisk\":%s,"
              "\"hasProtectedDisk\":%s,\"hasUnprotectedDisk\":%s,\"motor\":%s,"
              "\"writing\":%s}",
              (int)info->nr, (int)info->head.cylinder, (int)info->head.head,
              info->isConnected ? "true" : "false",
              info->hasDisk ? "true" : "false",
              info->hasUnmodifiedDisk ? "true" : "false",
              info->hasModifiedDisk ? "true" : "false",
              info->hasProtectedDisk ? "true" : "false",
              info->hasUnprotectedDisk ? "true" : "false",
              info->motor ? "true" : "false",
              info->writing ? "true" : "false");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_harddrive_info(int drive_num) {
  static char buffer[1024];
  try {
    const HardDriveInfo *info = nullptr;
    switch (drive_num) {
    case 0:
      info = &wrapper->emu->hd0.getInfo();
      break;
    case 1:
      info = &wrapper->emu->hd1.getInfo();
      break;
    case 2:
      info = &wrapper->emu->hd2.getInfo();
      break;
    case 3:
      info = &wrapper->emu->hd3.getInfo();
      break;
    default:
      return "{\"error\":\"invalid_drive\"}";
    }
    snprintf(
        buffer, sizeof(buffer),
        "{\"nr\":%d,\"isConnected\":%s,\"isCompatible\":%s,\"hasDisk\":%s,"
        "\"hasUnmodifiedDisk\":%s,\"hasModifiedDisk\":%s,"
        "\"hasProtectedDisk\":%s,\"hasUnprotectedDisk\":%s,\"partitions\":%d,"
        "\"writeProtected\":%s,\"modified\":%s,\"cylinder\":%d,\"head\":%d}",
        (int)info->nr, info->isConnected ? "true" : "false",
        info->isCompatible ? "true" : "false",
        info->hasDisk ? "true" : "false",
        info->hasUnmodifiedDisk ? "true" : "false",
        info->hasModifiedDisk ? "true" : "false",
        info->hasProtectedDisk ? "true" : "false",
        info->hasUnprotectedDisk ? "true" : "false", (int)info->partitions,
        info->writeProtected ? "true" : "false",
        info->modified ? "true" : "false", (int)info->head.cylinder,
        (int)info->head.head);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_copper_info() {
  static char buffer[512];
  try {
    const auto &info = wrapper->emu->agnus.copper.getInfo();
    snprintf(buffer, sizeof(buffer),
              "{\"copList\":%d,\"copList1Start\":\"0x%08X\",\"copList1End\":"
              "\"0x%08X\",\"copList2Start\":\"0x%08X\",\"copList2End\":\"0x%"
              "08X\",\"active\":%s,\"cdang\":%s,\"coppc0\":\"0x%08X\","
              "\"cop1lc\":\"0x%08X\",\"cop2lc\":\"0x%08X\",\"cop1ins\":\"0x%"
              "04X\",\"cop2ins\":\"0x%04X\"}",
              (int)info.copList, info.copList1Start, info.copList1End,
              info.copList2Start, info.copList2End,
              info.active ? "true" : "false", info.cdang ? "true" : "false",
              info.coppc0, info.cop1lc, info.cop2lc, info.cop1ins,
              info.cop2ins);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_blitter_info() {
  static char buffer[1024];
  try {
    const auto &info = wrapper->emu->agnus.blitter.getInfo();
    snprintf(
        buffer, sizeof(buffer),
        "{\"bltcon0\":\"0x%04X\",\"bltcon1\":\"0x%04X\",\"ash\":%d,\"bsh\":%"
        "d,\"minterm\":\"0x%04X\",\"bltapt\":\"0x%08X\",\"bltbpt\":\"0x%"
        "08X\",\"bltcpt\":\"0x%08X\",\"bltdpt\":\"0x%08X\",\"bltafwm\":\"0x%"
        "04X\",\"bltalwm\":\"0x%04X\",\"bbusy\":%s,\"bzero\":%s}",
        info.bltcon0, info.bltcon1, info.ash, info.bsh, info.minterm,
        info.bltapt, info.bltbpt, info.bltcpt, info.bltdpt, info.bltafwm,
        info.bltalwm, info.bbusy ? "true" : "false",
        info.bzero ? "true" : "false");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_serial_port_info() {
  static char buffer[512];
  try {
    const auto &info = wrapper->emu->serialPort.getInfo();
    snprintf(buffer, sizeof(buffer),
              "{\"port\":\"0x%08X\",\"txd\":%s,\"rxd\":%s,\"rts\":%s,\"cts\":%"
              "s,\"dsr\":%s,\"cd\":%s,\"dtr\":%s}",
              info.port, info.txd ? "true" : "false",
              info.rxd ? "true" : "false", info.rts ? "true" : "false",
              info.cts ? "true" : "false", info.dsr ? "true" : "false",
              info.cd ? "true" : "false", info.dtr ? "true" : "false");
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_keyboard_info() {
  static char buffer[512];
  try {
    const auto &info = wrapper->emu->keyboard.getInfo();
    snprintf(buffer, sizeof(buffer), "{\"state\":%d,\"shiftReg\":\"0x%02X\"}",
              (int)info.state, info.shiftReg);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_control_port_info(int port) {
  static char buffer[512];
  try {
    const ControlPortInfo *info = nullptr;
    if (port == 1) {
      info = &wrapper->emu->controlPort1.getInfo();
    } else if (port == 2) {
      info = &wrapper->emu->controlPort2.getInfo();
    } else {
      return "{\"error\":\"invalid_port\"}";
    }
    snprintf(
        buffer, sizeof(buffer),
        "{\"m0v\":%s,\"m0h\":%s,\"m1v\":%s,\"m1h\":%s,\"joydat\":\"0x%04X\","
        "\"potgo\":\"0x%04X\",\"potgor\":\"0x%04X\",\"potdat\":\"0x%04X\"}",
        info->m0v ? "true" : "false", info->m0h ? "true" : "false",
        info->m1v ? "true" : "false", info->m1h ? "true" : "false",
        info->joydat, info->potgo, info->potgor, info->potdat);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}

extern "C" const char *wasm_get_disk_controller_info() {
  static char buffer[512];
  try {
    const auto &info = wrapper->emu->paula.diskController.getInfo();
    snprintf(buffer, sizeof(buffer),
              "{\"selectedDrive\":%d,\"state\":%d,\"fifoCount\":%d,\"dsklen\":"
              "\"0x%04X\",\"dskbytr\":\"0x%04X\",\"dsksync\":\"0x%04X\","
              "\"prb\":\"0x%02X\"}",
              (int)info.selectedDrive, (int)info.state, info.fifoCount,
              info.dsklen, info.dskbytr, info.dsksync, info.prb);
    return buffer;
  } catch (...) {
    return "{\"error\":true}";
  }
}


// Custom:

extern "C" const char *wasm_get_all_custom_registers() {
  static std::string result_buffer;
  char hex_buf[16];

  auto *agnus = wrapper->emu->agnus.agnus;
  auto *denise = wrapper->emu->denise.denise;

  result_buffer = "{";

  // Beam position - read from VPOSR/VHPOSR ($004/$006)
  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x004));
  result_buffer += "\"VPOS\":{\"addr\":\"0x004\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x006));
  result_buffer += "\"VHPOS\":{\"addr\":\"0x006\",\"value\":" + std::string(hex_buf) + "},";

  // DSKDAT ($008) - disk data early read (read addr, unified name)
  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x008));
  result_buffer += "\"DSKDAT\":{\"addr\":\"0x008\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x00A));
  result_buffer += "\"JOY0DAT\":{\"addr\":\"0x00A\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x00C));
  result_buffer += "\"JOY1DAT\":{\"addr\":\"0x00C\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x00E));
  result_buffer += "\"CLXDAT\":{\"addr\":\"0x00E\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x012));
  result_buffer += "\"POT0DAT\":{\"addr\":\"0x012\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x014));
  result_buffer += "\"POT1DAT\":{\"addr\":\"0x014\",\"value\":" + std::string(hex_buf) + "},";

  // POTGO ($016) - read addr for pot, unified name with write reg
  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x016));
  result_buffer += "\"POTGO\":{\"addr\":\"0x016\",\"value\":" + std::string(hex_buf) + "},";

  // SERDAT ($018) - serial data read addr, unified name with write reg
  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x018));
  result_buffer += "\"SERDAT\":{\"addr\":\"0x018\",\"value\":" + std::string(hex_buf) + "},";

  // DSKBYT ($01A) - disk byte/status read (read addr, R suffix stripped)
  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->mem.mem->spypeekCustom16(0x01A));
  result_buffer += "\"DSKBYT\":{\"addr\":\"0x01A\",\"value\":" + std::string(hex_buf) + "},";

  // DSKPT ($020) - disk pointer (32-bit)
  sprintf(hex_buf, "\"0x%08X\"", agnus->dskpt);
  result_buffer += "\"DSKPT\":{\"addr\":\"0x020\",\"value\":" + std::string(hex_buf) + "},";

  // Disk controller registers
  auto diskInfo = wrapper->emu->paula.diskController.getInfo();
  sprintf(hex_buf, "\"0x%04X\"", diskInfo.dsklen);
  result_buffer += "\"DSKLEN\":{\"addr\":\"0x024\",\"value\":" + std::string(hex_buf) + "},";

  // COPCON ($02E) - coprocessor control (cdang = bit 1)
  auto copperInfo = wrapper->emu->agnus.copper.getInfo();
  sprintf(hex_buf, "\"0x%04X\"", copperInfo.cdang ? 0x0002 : 0x0000);
  result_buffer += "\"COPCON\":{\"addr\":\"0x02E\",\"value\":" + std::string(hex_buf) + "},";

  // SERPER ($032) - serial port period and control
  sprintf(hex_buf, "\"0x%04X\"", wrapper->emu->paula.uart.getInfo().serper);
  result_buffer += "\"SERPER\":{\"addr\":\"0x032\",\"value\":" + std::string(hex_buf) + "},";

  // Blitter registers ($040-$074)
  auto blitterInfo = wrapper->emu->agnus.blitter.getInfo();
  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.bltcon0);
  result_buffer += "\"BLTCON0\":{\"addr\":\"0x040\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.bltcon1);
  result_buffer += "\"BLTCON1\":{\"addr\":\"0x042\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.bltafwm);
  result_buffer += "\"BLTAFWM\":{\"addr\":\"0x044\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.bltalwm);
  result_buffer += "\"BLTALWM\":{\"addr\":\"0x046\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", blitterInfo.bltcpt);
  result_buffer += "\"BLTCPT\":{\"addr\":\"0x048\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", blitterInfo.bltbpt);
  result_buffer += "\"BLTBPT\":{\"addr\":\"0x04C\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", blitterInfo.bltapt);
  result_buffer += "\"BLTAPT\":{\"addr\":\"0x050\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", blitterInfo.bltdpt);
  result_buffer += "\"BLTDPT\":{\"addr\":\"0x054\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", (u16)blitterInfo.bltcmod);
  result_buffer += "\"BLTCMOD\":{\"addr\":\"0x060\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", (u16)blitterInfo.bltbmod);
  result_buffer += "\"BLTBMOD\":{\"addr\":\"0x062\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", (u16)blitterInfo.bltamod);
  result_buffer += "\"BLTAMOD\":{\"addr\":\"0x064\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", (u16)blitterInfo.bltdmod);
  result_buffer += "\"BLTDMOD\":{\"addr\":\"0x066\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.chold);
  result_buffer += "\"BLTCDAT\":{\"addr\":\"0x070\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.bhold);
  result_buffer += "\"BLTBDAT\":{\"addr\":\"0x072\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.ahold);
  result_buffer += "\"BLTADAT\":{\"addr\":\"0x074\",\"value\":" + std::string(hex_buf) + "},";

  // BLTDDAT ($000) - blitter destination early read
  sprintf(hex_buf, "\"0x%04X\"", blitterInfo.dhold);
  result_buffer += "\"BLTDDAT\":{\"addr\":\"0x000\",\"value\":" + std::string(hex_buf) + "},";

  // DENISEID ($07C) - Denise chip revision
  sprintf(hex_buf, "\"0x%04X\"", denise->spypeekDENISEID());
  result_buffer += "\"DENISEID\":{\"addr\":\"0x07C\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", diskInfo.dsksync);
  result_buffer += "\"DSKSYNC\":{\"addr\":\"0x07E\",\"value\":" + std::string(hex_buf) + "},";

  // Copper registers ($080-$086)
  sprintf(hex_buf, "\"0x%08X\"", copperInfo.cop1lc);
  result_buffer += "\"COP1LC\":{\"addr\":\"0x080\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", copperInfo.cop2lc);
  result_buffer += "\"COP2LC\":{\"addr\":\"0x084\",\"value\":" + std::string(hex_buf) + "},";

  // Display window and fetch registers ($08E-$094)
  sprintf(hex_buf, "\"0x%04X\"", denise->diwstrt);
  result_buffer += "\"DIWSTRT\":{\"addr\":\"0x08E\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->diwstop);
  result_buffer += "\"DIWSTOP\":{\"addr\":\"0x090\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", agnus->sequencer.diwhigh);
  result_buffer += "\"DIWHIGH\":{\"addr\":\"0x1E4\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", agnus->sequencer.ddfstrt);
  result_buffer += "\"DDFSTRT\":{\"addr\":\"0x092\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", agnus->sequencer.ddfstop);
  result_buffer += "\"DDFSTOP\":{\"addr\":\"0x094\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", agnus->dmacon);
  result_buffer += "\"DMACON\":{\"addr\":\"0x096\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->clxcon);
  result_buffer += "\"CLXCON\":{\"addr\":\"0x098\",\"value\":" + std::string(hex_buf) + "},";

  // Paula registers ($09A-$09E)
  auto paulaInfo = wrapper->emu->paula.getInfo();
  sprintf(hex_buf, "\"0x%04X\"", paulaInfo.intena);
  result_buffer += "\"INTENA\":{\"addr\":\"0x09A\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", paulaInfo.intreq);
  result_buffer += "\"INTREQ\":{\"addr\":\"0x09C\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", paulaInfo.adkcon);
  result_buffer += "\"ADKCON\":{\"addr\":\"0x09E\",\"value\":" + std::string(hex_buf) + "},";

  // Audio channel location pointers and registers ($0A0-$0DA)
  auto aud0Info = wrapper->emu->paula.audioChannel0.getInfo();
  auto aud1Info = wrapper->emu->paula.audioChannel1.getInfo();
  auto aud2Info = wrapper->emu->paula.audioChannel2.getInfo();
  auto aud3Info = wrapper->emu->paula.audioChannel3.getInfo();

  // Audio Channel 0 ($0A0-$0AA)
  sprintf(hex_buf, "\"0x%08X\"", agnus->audlc[0]);
  result_buffer += "\"AUD0LC\":{\"addr\":\"0x0A0\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud0Info.audlenLatch);
  result_buffer += "\"AUD0LEN\":{\"addr\":\"0x0A4\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud0Info.audperLatch);
  result_buffer += "\"AUD0PER\":{\"addr\":\"0x0A6\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud0Info.audvolLatch);
  result_buffer += "\"AUD0VOL\":{\"addr\":\"0x0A8\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud0Info.auddat);
  result_buffer += "\"AUD0DAT\":{\"addr\":\"0x0AA\",\"value\":" + std::string(hex_buf) + "},";

  // Audio Channel 1 ($0B0-$0BA)
  sprintf(hex_buf, "\"0x%08X\"", agnus->audlc[1]);
  result_buffer += "\"AUD1LC\":{\"addr\":\"0x0B0\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud1Info.audlenLatch);
  result_buffer += "\"AUD1LEN\":{\"addr\":\"0x0B4\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud1Info.audperLatch);
  result_buffer += "\"AUD1PER\":{\"addr\":\"0x0B6\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud1Info.audvolLatch);
  result_buffer += "\"AUD1VOL\":{\"addr\":\"0x0B8\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud1Info.auddat);
  result_buffer += "\"AUD1DAT\":{\"addr\":\"0x0BA\",\"value\":" + std::string(hex_buf) + "},";

  // Audio Channel 2 ($0C0-$0CA)
  sprintf(hex_buf, "\"0x%08X\"", agnus->audlc[2]);
  result_buffer += "\"AUD2LC\":{\"addr\":\"0x0C0\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud2Info.audlenLatch);
  result_buffer += "\"AUD2LEN\":{\"addr\":\"0x0C4\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud2Info.audperLatch);
  result_buffer += "\"AUD2PER\":{\"addr\":\"0x0C6\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud2Info.audvolLatch);
  result_buffer += "\"AUD2VOL\":{\"addr\":\"0x0C8\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud2Info.auddat);
  result_buffer += "\"AUD2DAT\":{\"addr\":\"0x0CA\",\"value\":" + std::string(hex_buf) + "},";

  // Audio Channel 3 ($0D0-$0DA)
  sprintf(hex_buf, "\"0x%08X\"", agnus->audlc[3]);
  result_buffer += "\"AUD3LC\":{\"addr\":\"0x0D0\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud3Info.audlenLatch);
  result_buffer += "\"AUD3LEN\":{\"addr\":\"0x0D4\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud3Info.audperLatch);
  result_buffer += "\"AUD3PER\":{\"addr\":\"0x0D6\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud3Info.audvolLatch);
  result_buffer += "\"AUD3VOL\":{\"addr\":\"0x0D8\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", aud3Info.auddat);
  result_buffer += "\"AUD3DAT\":{\"addr\":\"0x0DA\",\"value\":" + std::string(hex_buf) + "},";

  // Bitplane pointers ($0E0-$0F4) - 32-bit values
  sprintf(hex_buf, "\"0x%08X\"", agnus->bplpt[0]);
  result_buffer += "\"BPL1PT\":{\"addr\":\"0x0E0\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->bplpt[1]);
  result_buffer += "\"BPL2PT\":{\"addr\":\"0x0E4\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->bplpt[2]);
  result_buffer += "\"BPL3PT\":{\"addr\":\"0x0E8\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->bplpt[3]);
  result_buffer += "\"BPL4PT\":{\"addr\":\"0x0EC\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->bplpt[4]);
  result_buffer += "\"BPL5PT\":{\"addr\":\"0x0F0\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->bplpt[5]);
  result_buffer += "\"BPL6PT\":{\"addr\":\"0x0F4\",\"value\":" + std::string(hex_buf) + "},";

  // Bitplane control and modulo registers ($100-$10A)
  sprintf(hex_buf, "\"0x%04X\"", denise->bplcon0);
  result_buffer += "\"BPLCON0\":{\"addr\":\"0x100\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bplcon1);
  result_buffer += "\"BPLCON1\":{\"addr\":\"0x102\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bplcon2);
  result_buffer += "\"BPLCON2\":{\"addr\":\"0x104\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bplcon3);
  result_buffer += "\"BPLCON3\":{\"addr\":\"0x106\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", agnus->bpl1mod);
  result_buffer += "\"BPL1MOD\":{\"addr\":\"0x108\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", agnus->bpl2mod);
  result_buffer += "\"BPL2MOD\":{\"addr\":\"0x10A\",\"value\":" + std::string(hex_buf) + "},";

  // Bitplane data registers ($110-$11A)
  sprintf(hex_buf, "\"0x%04X\"", denise->bpldat[0]);
  result_buffer += "\"BPL1DAT\":{\"addr\":\"0x110\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bpldat[1]);
  result_buffer += "\"BPL2DAT\":{\"addr\":\"0x112\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bpldat[2]);
  result_buffer += "\"BPL3DAT\":{\"addr\":\"0x114\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bpldat[3]);
  result_buffer += "\"BPL4DAT\":{\"addr\":\"0x116\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bpldat[4]);
  result_buffer += "\"BPL5DAT\":{\"addr\":\"0x118\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%04X\"", denise->bpldat[5]);
  result_buffer += "\"BPL6DAT\":{\"addr\":\"0x11A\",\"value\":" + std::string(hex_buf) + "},";

  // Sprite pointers ($120-$13C) - 32-bit values
  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[0]);
  result_buffer += "\"SPR0PT\":{\"addr\":\"0x120\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[1]);
  result_buffer += "\"SPR1PT\":{\"addr\":\"0x124\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[2]);
  result_buffer += "\"SPR2PT\":{\"addr\":\"0x128\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[3]);
  result_buffer += "\"SPR3PT\":{\"addr\":\"0x12C\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[4]);
  result_buffer += "\"SPR4PT\":{\"addr\":\"0x130\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[5]);
  result_buffer += "\"SPR5PT\":{\"addr\":\"0x134\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[6]);
  result_buffer += "\"SPR6PT\":{\"addr\":\"0x138\",\"value\":" + std::string(hex_buf) + "},";

  sprintf(hex_buf, "\"0x%08X\"", agnus->sprpt[7]);
  result_buffer += "\"SPR7PT\":{\"addr\":\"0x13C\",\"value\":" + std::string(hex_buf) + "},";

  // Sprite data and position registers ($140-$17F)
  for (int i = 0; i < 8; i++) {
    char addr_str[8];
    sprintf(addr_str, "0x%03X", 0x140 + i * 8);
    sprintf(hex_buf, "\"0x%04X\"", denise->sprpos[i]);
    result_buffer += "\"SPR" + std::to_string(i) + "POS\":{\"addr\":\"" + addr_str + "\",\"value\":" + std::string(hex_buf) + "},";

    sprintf(addr_str, "0x%03X", 0x142 + i * 8);
    sprintf(hex_buf, "\"0x%04X\"", denise->sprctl[i]);
    result_buffer += "\"SPR" + std::to_string(i) + "CTL\":{\"addr\":\"" + addr_str + "\",\"value\":" + std::string(hex_buf) + "},";

    sprintf(addr_str, "0x%03X", 0x144 + i * 8);
    sprintf(hex_buf, "\"0x%04X\"", denise->sprdata[i]);
    result_buffer += "\"SPR" + std::to_string(i) + "DATA\":{\"addr\":\"" + addr_str + "\",\"value\":" + std::string(hex_buf) + "},";

    sprintf(addr_str, "0x%03X", 0x146 + i * 8);
    sprintf(hex_buf, "\"0x%04X\"", denise->sprdatb[i]);
    result_buffer += "\"SPR" + std::to_string(i) + "DATB\":{\"addr\":\"" + addr_str + "\",\"value\":" + std::string(hex_buf) + "},";
  }

  // Color palette ($180-$1BE) - all 32 colors (last entry, no trailing comma)
  for (int i = 0; i < 32; i++) {
    char addr_str[8];
    sprintf(addr_str, "0x%03X", 0x180 + (i * 2));
    char colorName[8];
    sprintf(colorName, "COLOR%02d", i);
    sprintf(hex_buf, "\"0x%04X\"", denise->pixelEngine.getColor(i));
    result_buffer += "\"" + std::string(colorName) + "\":{\"addr\":\"" + addr_str + "\",\"value\":" + std::string(hex_buf) + "}";
    if (i < 31) result_buffer += ",";
  }

  result_buffer += "}";

  return result_buffer.c_str();
}

extern "C" const char *wasm_set_custom_register(const char* reg_name, u32 value) {
  static char result_buffer[256];

  // Helper function for 16-bit register writes
  auto write16 = [&](u16 addr, u32 val) -> const char* {
    wrapper->emu->mem.mem->pokeCustom16<Accessor::CPU>(addr, val & 0xFFFF);
    snprintf(result_buffer, sizeof(result_buffer), "{\"success\":true,\"register\":\"%s\",\"value\":\"0x%04X\"}", reg_name, val & 0xFFFF);
    return result_buffer;
  };

  // Helper function for 32-bit register writes (split into high/low words)
  auto write32 = [&](u16 high_addr, u16 low_addr, u32 val) -> const char* {
    wrapper->emu->mem.mem->pokeCustom16<Accessor::CPU>(high_addr, (val >> 16) & 0xFFFF);
    wrapper->emu->mem.mem->pokeCustom16<Accessor::CPU>(low_addr, val & 0xFFFF);
    snprintf(result_buffer, sizeof(result_buffer), "{\"success\":true,\"register\":\"%s\",\"value\":\"0x%08X\"}", reg_name, val);
    return result_buffer;
  };

  try {
    std::string regName(reg_name);

    // 16-bit register lookup table
    static const std::unordered_map<std::string, u16> reg16_map = {
      // Main control registers
      {"DMACON", 0x096}, {"INTENA", 0x09A}, {"INTREQ", 0x09C}, {"ADKCON", 0x09E},
      // Copper control
      {"COPCON", 0x02E},
      // Serial port
      {"SERDAT", 0x030}, {"SERPER", 0x032},
      // Disk
      {"DSKLEN", 0x024}, {"DSKDAT", 0x026}, {"DSKSYNC", 0x07E},
      // Blitter control
      {"BLTCON0", 0x040}, {"BLTCON1", 0x042}, {"BLTAFWM", 0x044}, {"BLTALWM", 0x046},
      {"BLTSIZE", 0x058}, {"BLTCON0L", 0x05A}, {"BLTSIZV", 0x05C}, {"BLTSIZH", 0x05E},
      {"BLTCMOD", 0x060}, {"BLTBMOD", 0x062}, {"BLTAMOD", 0x064}, {"BLTDMOD", 0x066},
      {"BLTCDAT", 0x070}, {"BLTBDAT", 0x072}, {"BLTADAT", 0x074},
      // Display window
      {"DIWSTRT", 0x08E}, {"DIWSTOP", 0x090}, {"DDFSTRT", 0x092}, {"DDFSTOP", 0x094}, {"CLXCON", 0x098},
      // Display control
      {"BPLCON0", 0x100}, {"BPLCON1", 0x102}, {"BPLCON2", 0x104}, {"BPLCON3", 0x106},
      {"BPL1MOD", 0x108}, {"BPL2MOD", 0x10A},
      // Bitplane data
      {"BPL1DAT", 0x110}, {"BPL2DAT", 0x112}, {"BPL3DAT", 0x114},
      {"BPL4DAT", 0x116}, {"BPL5DAT", 0x118}, {"BPL6DAT", 0x11A},
      // Audio channels
      {"AUD0LEN", 0x0A4}, {"AUD0PER", 0x0A6}, {"AUD0VOL", 0x0A8}, {"AUD0DAT", 0x0AA},
      {"AUD1LEN", 0x0B4}, {"AUD1PER", 0x0B6}, {"AUD1VOL", 0x0B8}, {"AUD1DAT", 0x0BA},
      {"AUD2LEN", 0x0C4}, {"AUD2PER", 0x0C6}, {"AUD2VOL", 0x0C8}, {"AUD2DAT", 0x0CA},
      {"AUD3LEN", 0x0D4}, {"AUD3PER", 0x0D6}, {"AUD3VOL", 0x0D8}, {"AUD3DAT", 0x0DA},
    };

    // 32-bit register lookup table (high addr, low addr)
    static const std::unordered_map<std::string, std::pair<u16, u16>> reg32_map = {
      // Disk pointer
      {"DSKPT", {0x020, 0x022}},
      // Copper
      {"COP1LC", {0x080, 0x082}}, {"COP2LC", {0x084, 0x086}},
      // Blitter pointers
      {"BLTAPT", {0x050, 0x052}}, {"BLTBPT", {0x04C, 0x04E}},
      {"BLTCPT", {0x048, 0x04A}}, {"BLTDPT", {0x054, 0x056}},
      // Bitplane pointers
      {"BPL1PT", {0x0E0, 0x0E2}}, {"BPL2PT", {0x0E4, 0x0E6}}, {"BPL3PT", {0x0E8, 0x0EA}},
      {"BPL4PT", {0x0EC, 0x0EE}}, {"BPL5PT", {0x0F0, 0x0F2}}, {"BPL6PT", {0x0F4, 0x0F6}},
      // Sprite pointers
      {"SPR0PT", {0x120, 0x122}}, {"SPR1PT", {0x124, 0x126}}, {"SPR2PT", {0x128, 0x12A}},
      {"SPR3PT", {0x12C, 0x12E}}, {"SPR4PT", {0x130, 0x132}}, {"SPR5PT", {0x134, 0x136}},
      {"SPR6PT", {0x138, 0x13A}}, {"SPR7PT", {0x13C, 0x13E}},
      // Audio pointers
      {"AUD0LC", {0x0A0, 0x0A2}}, {"AUD1LC", {0x0B0, 0x0B2}},
      {"AUD2LC", {0x0C0, 0x0C2}}, {"AUD3LC", {0x0D0, 0x0D2}}
    };

    // Check 16-bit registers first
    auto reg16_it = reg16_map.find(regName);
    if (reg16_it != reg16_map.end()) {
      return write16(reg16_it->second, value);
    }

    // Check 32-bit registers
    auto reg32_it = reg32_map.find(regName);
    if (reg32_it != reg32_map.end()) {
      return write32(reg32_it->second.first, reg32_it->second.second, value);
    }

    // Handle sprite control and data registers dynamically
    if (regName.substr(0, 3) == "SPR" && regName.length() >= 7) {
      char sprite_num = regName[3];
      if (sprite_num >= '0' && sprite_num <= '7') {
        int sprite_idx = sprite_num - '0';
        std::string reg_type = regName.substr(4);

        if (reg_type == "POS") {
          return write16(0x140 + sprite_idx * 8, value);
        } else if (reg_type == "CTL") {
          return write16(0x142 + sprite_idx * 8, value);
        } else if (reg_type == "DATA") {
          return write16(0x144 + sprite_idx * 8, value);
        } else if (reg_type == "DATB") {
          return write16(0x146 + sprite_idx * 8, value);
        }
      }
    }

    // Handle color palette dynamically
    if (regName.substr(0, 5) == "COLOR" && regName.length() == 7) {
      std::string color_str = regName.substr(5, 2);
      int color_num = std::stoi(color_str);
      if (color_num >= 0 && color_num <= 31) {
        return write16(0x180 + (color_num * 2), value);
      }
    }

    // Error handling for read-only registers
    static const std::unordered_set<std::string> readonly_regs = {
      "JOY0DAT", "JOY1DAT", "POT0DAT", "POT1DAT", "CLXDAT",
      "VPOS", "VHPOS", "DSKBYT", "BLTDDAT", "DENISEID"
    };
    if (readonly_regs.count(regName)) {
      snprintf(result_buffer, sizeof(result_buffer), "{\"error\":true,\"message\":\"Read-only hardware register\",\"register\":\"%s\"}", reg_name);
      return result_buffer;
    }

    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":true,\"message\":\"Unknown or unsupported register\",\"register\":\"%s\"}", reg_name);
    return result_buffer;

  } catch (const std::exception& e) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":true,\"message\":\"Exception: %s\"}", e.what());
    return result_buffer;
  } catch (...) {
    snprintf(result_buffer, sizeof(result_buffer), "{\"error\":true,\"message\":\"Unknown error occurred\"}");
    return result_buffer;
  }
}

extern "C" const char *wasm_get_current_process() {
  static std::string result_buffer;

  try {
    // Get ExecBase from address 4 (like WinUAE)
    u32 execbase = wasm_peek32(4);

    // Get ThisTask from ExecBase + 276 (like WinUAE)
    u32 activetask = wasm_peek32(execbase + 276);
    if (!activetask) {
      result_buffer = "{\"error\":\"No active task\"}";
      return result_buffer.c_str();
    }

    // Check if it's a process (ln_Type == NT_PROCESS = 13)
    u8 tasktype =
        wrapper->emu->mem.mem->spypeek8<Accessor::CPU>(activetask + 8);
    if (tasktype != 13) {
      result_buffer = "{\"error\":\"Active task is not a process\"}";
      return result_buffer.c_str();
    }

    u32 cliPtr = wasm_peek32(activetask + 172);
    if (!cliPtr) {
      result_buffer = "{\"error\":\"Active process has no CLI\"}";
      return result_buffer.c_str();
    }
    // Convert BPTR to APTR (multiply by 4)
    u32 cli = cliPtr << 2;

    // Get task name from ln_Name (activetask + 10)
    u32 namePtr = wasm_peek32(activetask + 10);
    std::string taskName;
    if (namePtr) {
      // Read null-terminated string
      for (int i = 0; i < 64; i++) {
        u8 c = wrapper->emu->mem.mem->spypeek8<Accessor::CPU>(namePtr + i);
        if (c == 0)
          break;
        taskName += (char)c;
      }
    }

    result_buffer = "{";
    result_buffer += "\"address\":" + std::to_string(activetask) + ",";
    result_buffer += "\"name\":\"" + taskName + "\"";

    // Get CLI command name (cli + 16)
    u32 cmdPtr = wasm_peek32(cli + 16);
    if (cmdPtr) {
      u32 cmdAddr = cmdPtr << 2; // BPTR to APTR
      u8 cmdLen = wrapper->emu->mem.mem->spypeek8<Accessor::CPU>(cmdAddr);
      std::string command;
      for (int i = 0; i < cmdLen && i < 63; i++) {
        command += (char)wrapper->emu->mem.mem->spypeek8<Accessor::CPU>(
            cmdAddr + 1 + i);
      }
      result_buffer += ",\"command\":\"" + command + "\"";
    }

    // Get seglist (cli + 60)
    u32 seglistPtr = wasm_peek32(cli + 60);
    if (seglistPtr) {
      u32 seglistAddr = seglistPtr << 2;
      result_buffer += ",\"segments\":[";

      // Walk the seglist and include all segments
      u32 seglist = seglistAddr;
      bool firstSeg = true;
      while (seglist) {
        u32 size = wasm_peek32(seglist - 4) - 4;
        if (!firstSeg)
          result_buffer += ",";
        firstSeg = false;

        result_buffer += "{";
        result_buffer += "\"start\":" + std::to_string(seglist + 4) + ",";
        result_buffer += "\"size\":" + std::to_string(size);
        result_buffer += "}";

        u32 nextPtr = wasm_peek32(seglist);
        seglist = nextPtr ? (nextPtr << 2) : 0;

        // Safety check
        if (!firstSeg && seglist == seglistAddr)
          break;
      }
      result_buffer += "]";
    }

    result_buffer += "}";

  } catch (...) {
    result_buffer = "{\"error\":\"Failed to read current process\"}";
  }

  return result_buffer.c_str();
}

extern "C" const char *wasm_get_call_stack(u32 depth) {
  static std::string result_buffer;

  if (wrapper == NULL) {
    result_buffer = "{\"error\":\"Wrapper not initialized\"}";
    return result_buffer.c_str();
  }

  try {
    auto cpuInfo = wrapper->emu->cpu.getInfo();
    u32 stackPtr = cpuInfo.a[7]; // A7 is stack pointer

    // Limit depth to reasonable amount
    if (depth > 64)
      depth = 64;
    if (depth == 0)
      depth = 16; // Default depth

    result_buffer = "[";
    result_buffer += "\"callStack\":[";

    bool first = true;

    // Scan stack for likely return addresses (stack grows downward,
    // word-aligned) Scan in 2-byte increments since stack is word-aligned
    for (u32 i = 0; i < depth * 2; i++) {
      u32 addr = stackPtr + (i * 2);

      // Try reading as 32-bit address first
      u32 value = 0;
      bool foundValue = false;

      // Check if we can read 32-bit value (most return addresses are 32-bit)
      if (i % 2 == 0 &&
          i < (depth * 2 - 1)) { // Make sure we don't read past our scan limit
        try {
          u16 high = wrapper->emu->mem.debugger.spypeek16(Accessor::CPU, addr);
          u16 low =
              wrapper->emu->mem.debugger.spypeek16(Accessor::CPU, addr + 2);
          value = (high << 16) | low;
          foundValue = true;
        } catch (...) {
          // Fall through to try 16-bit read
        }
      }

      // If 32-bit read failed or we're at an odd offset, try 16-bit
      if (!foundValue) {
        try {
          u16 word = wrapper->emu->mem.debugger.spypeek16(Accessor::CPU, addr);
          // Only consider 16-bit values that could be reasonable addresses
          if (word >= 0x1000) {
            value = word;
            foundValue = true;
          }
        } catch (...) {
          continue;
        }
      }

      if (!foundValue)
        continue;

      // Skip if value looks invalid (null, too low, or too high)
      if (value == 0 || value < 0x1000 || value > 0x2000000) {
        continue;
      }

      // Stop when we reach ROM addresses (Kickstart ROM area)
      // Amiga ROM is typically at 0xF80000-0xFFFFFF
      if (value >= 0xF80000) {
        break; // Stop tracing into ROM/OS code
      }

      // Check if this could be a return address by looking at the preceding
      // instruction
      try {
        // Return addresses point to instruction after JSR/BSR
        // JSR can be 2, 4, or 6 bytes depending on addressing mode
        // BSR is 2 or 4 bytes
        bool isReturnAddr = false;

        // Check 2 bytes back (for BSR.W or JSR with short addressing)
        u32 checkAddr = value - 2;
        u16 instr =
            wrapper->emu->mem.debugger.spypeek16(Accessor::CPU, checkAddr);
        if ((instr & 0xFF00) == 0x6100) { // BSR.W
          isReturnAddr = true;
        } else if ((instr & 0xFFC0) == 0x4E80) { // JSR
          isReturnAddr = true;
        }

        // Check 4 bytes back (for BSR.L or JSR with longer addressing)
        if (!isReturnAddr) {
          checkAddr = value - 4;
          instr =
              wrapper->emu->mem.debugger.spypeek16(Accessor::CPU, checkAddr);
          if (instr == 0x61FF) { // BSR.L
            isReturnAddr = true;
          } else if ((instr & 0xFFC0) == 0x4E80) { // JSR variants
            isReturnAddr = true;
          }
        }

        // Check 6 bytes back (for JSR with absolute long addressing)
        if (!isReturnAddr) {
          checkAddr = value - 6;
          instr =
              wrapper->emu->mem.debugger.spypeek16(Accessor::CPU, checkAddr);
          if ((instr & 0xFFC0) == 0x4E80) { // JSR variants
            isReturnAddr = true;
          }
        }

        if (isReturnAddr) {
          if (!first)
            result_buffer += ",";
          first = false;

          result_buffer += std::to_string(value);
        }

      } catch (...) {
        // Skip if we can't read the preceding instruction
        continue;
      }
    }

    result_buffer += "]";
    result_buffer += "}";

  } catch (...) {
    result_buffer = "{\"error\":\"Failed to analyze call stack\"}";
  }

  return result_buffer.c_str();
}

extern "C" const char* wasm_get_current_message() {
    static std::string result_buffer;

    if (!hasLastMessage) {
        result_buffer = "{\"hasMessage\":false}";
        return result_buffer.c_str();
    }

    const char* msgName = vamiga::MsgEnum::key(lastMessage.type);

    result_buffer = "{";
    result_buffer += "\"hasMessage\":true,";
    result_buffer += "\"type\":" + std::to_string((int)lastMessage.type) + ",";
    result_buffer += "\"name\":\"" + std::string(msgName) + "\",";

    // Decode payload based on message type
    switch (lastMessage.type) {
        case vamiga::Msg::BREAKPOINT_REACHED:
        case vamiga::Msg::WATCHPOINT_REACHED:
        case vamiga::Msg::CATCHPOINT_REACHED:
        case vamiga::Msg::SWTRAP_REACHED:
        case vamiga::Msg::BEAMTRAP_REACHED:
        case vamiga::Msg::COPPERBP_REACHED:
        case vamiga::Msg::COPPERWP_REACHED:
        case vamiga::Msg::STEP:
            result_buffer += "\"payload\":{";
            result_buffer += "\"pc\":" + std::to_string(lastMessage.cpu.pc) + ",";
            result_buffer += "\"vector\":" + std::to_string(lastMessage.cpu.vector);
            result_buffer += "}";
            break;

        case vamiga::Msg::DRIVE_CONNECT:
        case vamiga::Msg::DRIVE_LED:
        case vamiga::Msg::DRIVE_MOTOR:
        case vamiga::Msg::DRIVE_STEP:
        case vamiga::Msg::DRIVE_POLL:
        case vamiga::Msg::DISK_INSERT:
        case vamiga::Msg::DISK_EJECT:
            result_buffer += "\"payload\":{";
            result_buffer += "\"nr\":" + std::to_string(lastMessage.drive.nr) + ",";
            result_buffer += "\"value\":" + std::to_string(lastMessage.drive.value) + ",";
            result_buffer += "\"volume\":" + std::to_string(lastMessage.drive.volume) + ",";
            result_buffer += "\"pan\":" + std::to_string(lastMessage.drive.pan);
            result_buffer += "}";
            break;

        case vamiga::Msg::VIEWPORT:
            result_buffer += "\"payload\":{";
            result_buffer += "\"hstrt\":" + std::to_string(lastMessage.viewport.hstrt) + ",";
            result_buffer += "\"vstrt\":" + std::to_string(lastMessage.viewport.vstrt) + ",";
            result_buffer += "\"hstop\":" + std::to_string(lastMessage.viewport.hstop) + ",";
            result_buffer += "\"vstop\":" + std::to_string(lastMessage.viewport.vstop);
            result_buffer += "}";
            break;

        default:
            // For simple value messages
            result_buffer += "\"payload\":{";
            result_buffer += "\"value1\":" + std::to_string(lastMessage.value) + ",";
            result_buffer += "\"value2\":" + std::to_string(lastMessage.value2);
            result_buffer += "}";
            break;
    }

    result_buffer += "}";

    return result_buffer.c_str();
}

extern "C" void wasm_clear_current_message() {
    hasLastMessage = false;
}
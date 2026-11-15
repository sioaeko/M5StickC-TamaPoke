// Host-only hardware double: executes the actual port UI, renders into a PPM.
#pragma once
#include "../../shim/Arduino.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <cassert>
#define PROGMEM
#include "lgfx/Fonts/glcdfont.h"
struct DeviceSerialClass:MockSerial { int available(){return 0;} int read(){return -1;} };
static DeviceSerialClass DeviceSerial;
#define Serial DeviceSerial
struct String:std::string {
  using std::string::string;
  using std::string::operator=;
  bool startsWith(const char* s) const {return find(s)==0;}
};
inline void pinMode(int,int){} inline void digitalWrite(int,int){}
inline void delay(int n){mockAdvanceMillis(n);}
inline uint32_t micros(){return millis()*1000u;}
inline uint32_t getCpuFrequencyMhz(){return 80;}
inline unsigned esp_random(){return 123;}
constexpr int OUTPUT=1,HIGH=1;
template<class T> T constrain(T v,int lo,int hi){return v<lo?lo:v>hi?hi:v;}
struct EspMock {unsigned getFlashChipSize(){return 8388608;} unsigned getPsramSize(){return 2097152;} unsigned getFreeHeap(){return 200000;}};
static EspMock ESP;
namespace m5 {
struct rtc_datetime_t {
  struct {int year=2026,month=11,date=16,weekDay=1;} date;
  struct {int hours=12,minutes=0,seconds=0;} time;
};
}
struct ButtonMock {
  bool click=false,hold=false;
  bool wasClicked(){bool v=click;click=false;return v;}
  bool wasHold(){bool v=hold;hold=false;return v;}
  bool down=false; bool isPressed(){return down;}
  void setHoldThresh(int){}
};
struct DisplayMock {
  int rotation=1,brightness=120; bool asleep=false;
  void sleep(){asleep=true;} void wakeup(){asleep=false;}
  void setRotation(int n){rotation=n;} void setBrightness(int n){brightness=n;}
  void fillScreen(int){} void println(const char*){}
};
struct M5Mock {
  DisplayMock Display;
  ButtonMock BtnA,BtnB,BtnPWR;
  struct RtcMock {
    m5::rtc_datetime_t dt;
    bool isEnabled(){return true;} bool getVoltLow(){return false;}
    m5::rtc_datetime_t getDateTime(){return dt;}
    bool setDateTime(m5::rtc_datetime_t v){dt=v;return true;}
  } Rtc;
  struct {int getBatteryLevel(){return 84;} bool isCharging(){return false;} void powerOff(){} } Power;
  struct Config {bool internal_imu=false,internal_mic=false,internal_spk=true,clear_display=true;};
  Config config(){return {};}
  void begin(Config){} void update(){}
};
static M5Mock M5;
class M5Canvas {
  std::vector<uint16_t> px=std::vector<uint16_t>(240*135);
  uint16_t ink=0xffff; int scale=1;
  void pixel(int x,int y,int c){if(x>=0&&x<240&&y>=0&&y<135)px[y*240+x]=c;}
public:
  M5Canvas(DisplayMock*){}
  void setColorDepth(int){} bool createSprite(int,int){return true;}
  void setTextFont(int){} void setTextWrap(bool){} void pushSprite(int,int){}
  void setTextColor(int c){ink=c;} void setTextSize(int n){scale=n;}
  void fillRect(int x,int y,int w,int h,int c){for(int j=y;j<y+h;++j)for(int i=x;i<x+w;++i)pixel(i,j,c);}
  // corner test shared by the rounded-rect fill and outline
  static bool inRound(int i,int j,int w,int h,int r){
    r=std::min(r,std::min(w,h)/2);
    int cx=i<r?r:i>=w-r?w-r-1:i, cy=j<r?r:j>=h-r?h-r-1:j;
    return (i-cx)*(i-cx)+(j-cy)*(j-cy)<=r*r;
  }
  void fillRoundRect(int x,int y,int w,int h,int r,int c){for(int j=0;j<h;++j)for(int i=0;i<w;++i)if(inRound(i,j,w,h,r))pixel(x+i,y+j,c);}
  void drawRoundRect(int x,int y,int w,int h,int r,int c){for(int j=0;j<h;++j)for(int i=0;i<w;++i)if(inRound(i,j,w,h,r)&&!(inRound(i-1,j,w,h,r)&&inRound(i+1,j,w,h,r)&&inRound(i,j-1,w,h,r)&&inRound(i,j+1,w,h,r)&&i>0&&j>0&&i<w-1&&j<h-1))pixel(x+i,y+j,c);}
  void drawRect(int x,int y,int w,int h,int c){drawRoundRect(x,y,w,h,0,c);}
  void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,int c){
    int minx=std::min({x0,x1,x2}),maxx=std::max({x0,x1,x2}),miny=std::min({y0,y1,y2}),maxy=std::max({y0,y1,y2});
    auto e=[](int ax,int ay,int bx,int by,int px,int py){return (bx-ax)*(py-ay)-(by-ay)*(px-ax);};
    for(int y=miny;y<=maxy;++y)for(int x=minx;x<=maxx;++x){int a=e(x0,y0,x1,y1,x,y),b=e(x1,y1,x2,y2,x,y),d=e(x2,y2,x0,y0,x,y);
      if((a>=0&&b>=0&&d>=0)||(a<=0&&b<=0&&d<=0))pixel(x,y,c);}
  }
  void fillScreen(int c){std::fill(px.begin(),px.end(),c);}
  void fillEllipse(int x,int y,int rx,int ry,int c){for(int j=-ry;j<=ry;++j)for(int i=-rx;i<=rx;++i)if(double(i*i)/(rx*rx)+double(j*j)/(ry*ry)<=1)pixel(x+i,y+j,c);}
  void fillCircle(int x,int y,int r,int c){fillEllipse(x,y,r,r,c);}
  void drawCircle(int x,int y,int r,int c){for(int i=0;i<360;++i)pixel(x+int(r*cos(i*.0174533)),y+int(r*sin(i*.0174533)),c);}
  void drawLine(int x,int y,int x2,int y2,int c){int n=std::max(abs(x2-x),abs(y2-y));for(int i=0;i<=n;++i)pixel(x+(x2-x)*i/std::max(1,n),y+(y2-y)*i/std::max(1,n),c);}
  void drawString(const char* s,int x,int y){
    assert(x>=0&&y>=0&&x+int(strlen(s))*6*scale<=240&&y+8*scale<=135);
    for(;*s;++s,x+=6*scale)for(int col=0;col<5;++col)for(int row=0;row<8;++row)
      if(font[(uint8_t)*s*5+col]&(1<<row))fillRect(x+col*scale,y+row*scale,scale,scale,ink);
  }
  void exportPPM(const char* path){std::ofstream f(path,std::ios::binary);f<<"P6\n240 135\n255\n";for(auto c:px){char rgb[]={char(((c>>11)&31)*255/31),char(((c>>5)&63)*255/63),char((c&31)*255/31)};f.write(rgb,3);}}
};

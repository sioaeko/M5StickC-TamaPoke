#pragma once
#include <M5Unified.h>
#include <LittleFS.h>
#include "sprite_format.h"

class CompactMon {
  uint8_t* data=nullptr;
  int cached=-1;
  bool ok=false;
  stick::SpriteView view;
public:
  ~CompactMon() { free(data); }
  void release() { free(data); data=nullptr; cached=-1; ok=false; }
  bool load(int dex, bool shiny) {
    int key=dex+(shiny?151:0);
    if(key==cached) return ok;
    cached=key; ok=false;
    free(data); data=nullptr;
    if(dex<1 || dex>151) return false;
    auto f=LittleFS.open("/monsters.pack","r");
    uint8_t head[8];
    if(!f || f.read(head,4)!=4 || memcmp(head,"SPM1",4)) return false;
    if(!f.seek(4+(key-1)*4) || f.read(head,8)!=8) return false;
    uint32_t begin=stick::u32(head), end=stick::u32(head+4);
    if(begin<1216 || end<=begin || end>f.size() || end-begin>2*stick::MaxDecoded+4) return false;
    size_t bytes=end-begin;
    uint8_t* packed=(uint8_t*)malloc(bytes);
    if(!packed) return false;
    bool read=f.seek(begin) && f.read(packed,bytes)==bytes;
    uint32_t size=read && bytes>=4 ? stick::u32(packed):0;
    if(size && size<=stick::MaxDecoded) {
      data=(uint8_t*)malloc(size);
      ok=data && stick::decode(packed,bytes,data,size) && stick::parse(data,size,view);
    }
    free(packed);
    if(!ok) { free(data); data=nullptr; }
    return ok;
  }
  // sil=true paints every opaque pixel in one ink colour, like the original
  // firmware's silhouettes for Pokedex entries that were never raised.
  void draw(M5Canvas& c, int centerX, int groundY, unsigned action, uint32_t now, int scale=0,
            bool sil=false, uint16_t ink=0x18C4, int maxScale=3, int maxH=48) {
    if(!ok) {
      c.drawCircle(centerX,groundY-15,14,ink);
      c.setTextColor(ink); c.setTextSize(1);
      c.drawString("?",centerX-2,groundY-19);
      return;
    }
    if(action>=12 || !view.acts[action].pixels) action=0;
    const auto& a=view.acts[action];
    if(!scale) scale=max(1,min(maxScale,min(maxH/int(a.h),104/int(a.w))));
    const uint8_t* p=a.pixels+stick::frameAt(a,now)*a.w*a.h;
    int x0=centerX-a.w*scale/2, y0=groundY-a.h*scale;
    for(int y=0;y<a.h;++y,p+=a.w) for(int x=0;x<a.w;) {
      uint8_t v=p[x];
      int run=1;
      while(x+run<a.w && (sil ? (p[x+run]!=255)==(v!=255) : p[x+run]==v)) ++run;
      if(v!=255) c.fillRect(x0+x*scale,y0+y*scale,run*scale,scale,sil?ink:view.palette[v]);
      x+=run;
    }
  }
};

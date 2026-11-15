#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace stick {
constexpr uint32_t MaxDecoded = 180000;
inline uint16_t u16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t* p) { return u16(p) | uint32_t(u16(p+2)) << 16; }

struct Action {
  uint8_t w=0, h=0, frames=0;
  uint16_t ms[6] = {};
  const uint8_t* pixels=nullptr;
};
struct SpriteView {
  uint16_t colors=0;
  uint16_t palette[256] = {};
  Action acts[12];
};
// Pure parser shared by the device and host corruption tests.
inline bool decode(const uint8_t* src, size_t size, uint8_t* dst, size_t capacity) {
  if (size < 6 || size % 2 || u32(src) > capacity || u32(src) > MaxDecoded) return false;
  size_t used=0, expected=u32(src);
  for (size_t i=4; i<size; i+=2) {
    size_t n=src[i];
    if (!n || n > expected-used) return false;
    memset(dst+used, src[i+1], n);
    used+=n;
  }
  return used==expected;
}
inline bool parse(const uint8_t* src, size_t size, SpriteView& out) {
  out = SpriteView{};
  if (size < 7 || memcmp(src,"TPK2",4) || src[4] > 12) return false;
  out.colors=u16(src+5);
  if (!out.colors || out.colors > 255 || size < 7+2u*out.colors) return false;
  for (unsigned i=0;i<out.colors;++i) out.palette[i]=u16(src+7+2*i);
  size_t p=7+2*out.colors;
  for (unsigned i=0;i<src[4];++i) {
    if (size-p<4) return false;
    unsigned id=src[p++], w=src[p++], h=src[p++], nf=src[p++];
    if (id>=12 || out.acts[id].pixels || !w || !h || w>48 || h>48 || !nf || nf>6) return false;
    size_t bytes=w*h*nf;
    if (size-p < 2*nf+bytes) return false;
    auto& a=out.acts[id];
    a.w=w; a.h=h; a.frames=nf;
    for(unsigned j=0;j<nf;++j) { a.ms[j]=u16(src+p); p+=2; if(!a.ms[j]) return false; }
    a.pixels=src+p;
    for(size_t j=0;j<bytes;++j) if(src[p+j]!=255 && src[p+j]>=out.colors) return false;
    p+=bytes;
  }
  return p==size && out.acts[0].pixels;
}
inline unsigned frameAt(const Action& a, uint32_t now) {
  uint32_t total=0;
  for(unsigned i=0;i<a.frames;++i) total+=a.ms[i];
  if(!total) return 0;
  now%=total;
  for(unsigned i=0;i<a.frames;++i) { if(now<a.ms[i]) return i; now-=a.ms[i]; }
  return 0;
}
}

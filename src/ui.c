#include "ui.h"
#include <string.h>

/* Compact 5x7 uppercase/digit font. Each byte is one 5-bit row. */
static const uint8_t *glyph(char c)
{
    static const uint8_t blank[7]={0,0,0,0,0,0,0};
    static const uint8_t punct[][7]={
        {0,0,0,0,0,0,0}, {0,4,4,4,4,0,4}, {0,0,0,31,0,0,0},
        {0,0,0,0,0,4,4}, {0,1,2,4,8,16,0}, {0,0,0,0,0,0,4}
    };
    static const uint8_t digits[10][7]={
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}};
    static const uint8_t letters[26][7]={
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
    if(c>='0'&&c<='9') return digits[c-'0'];
    if(c>='a'&&c<='z') c=(char)(c-'a'+'A');
    if(c>='A'&&c<='Z') return letters[c-'A'];
    if(c=='!') return punct[1];
    if(c=='-') return punct[2];
    if(c==':') return punct[3];
    if(c=='/') return punct[4];
    if(c=='.') return punct[5];
    return blank;
}
void ui_fill_rect(uint8_t *fb,uint16_t stride,uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint8_t c)
{ uint16_t yy; for(yy=0;yy<h;yy++) memset(fb+(uint32_t)(y+yy)*stride+x,c,w); }
void ui_draw_text(uint8_t *fb,uint16_t stride,uint16_t x,uint16_t y,const char *s,uint8_t c)
{ while(*s && x<314){ const uint8_t *g=glyph(*s++); uint8_t r,b; for(r=0;r<7;r++) for(b=0;b<5;b++) if(g[r]&(16>>b)) fb[(uint32_t)(y+r)*stride+x+b]=c; x+=6; } }
void ui_format_time(char out[9],uint32_t frame,uint32_t num,uint32_t den)
{ uint32_t sec=num? (uint32_t)(((uint64_t)frame*den)/num):0; uint32_t h=sec/3600,m=(sec/60)%60,s=sec%60; if(h>99)h=99; out[0]='0'+h/10;out[1]='0'+h%10;out[2]=':';out[3]='0'+m/10;out[4]='0'+m%10;out[5]=':';out[6]='0'+s/10;out[7]='0'+s%10;out[8]=0; }
void ui_draw_progress(uint8_t *fb,uint16_t stride,uint32_t current,uint32_t total,uint8_t ready,uint8_t slots,bool paused,uint32_t fps_num,uint32_t fps_den)
{ char a[9],b[9],buf[24]; uint16_t fill=total?(uint16_t)(((uint64_t)current*196)/total):0; ui_fill_rect(fb,stride,0,216,320,24,0); ui_format_time(a,current,fps_num,fps_den); ui_format_time(b,total,fps_num,fps_den); ui_draw_text(fb,stride,4,218,a,15); ui_draw_text(fb,stride,250,218,b,15); ui_fill_rect(fb,stride,62,220,200,5,8); ui_fill_rect(fb,stride,64,221,fill,3,15); buf[0]=paused?'P':'B';buf[1]=':';buf[2]='0'+ready;buf[3]='/';buf[4]='0'+slots;buf[5]=0; ui_draw_text(fb,stride,4,230,buf,15); }

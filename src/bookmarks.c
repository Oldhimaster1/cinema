#include "bookmarks.h"
#include <fileioc.h>
#include <string.h>
#define REC_BYTES 44
static uint32_t r32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void w32(uint8_t*p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
uint32_t cinema_movie_key(const cin2_header_t*h){uint8_t b[48],i;b[0]=h->width;b[1]=h->width>>8;b[2]=h->height;b[3]=h->height>>8;for(i=0;i<4;i++){b[4+i]=h->fps_num>>(8*i);b[8+i]=h->fps_den>>(8*i);b[12+i]=h->frame_count>>(8*i);}for(i=0;i<16;i++){b[16+2*i]=h->palette[i];b[17+2*i]=h->palette[i]>>8;}return cin2_crc32(b,48);}
void cinema_bookmarks_load(cinema_bookmarks_t*b,uint32_t key){uint8_t h,raw[REC_BYTES];uint32_t c;uint8_t i;memset(b,0,sizeof(*b));b->movie_key=key;h=ti_Open(CINEMA_BOOKMARK_APPVAR,"r");if(!h)return;if(ti_Read(raw,1,REC_BYTES,h)==REC_BYTES&&memcmp(raw,"C2BM",4)==0&&raw[4]==1&&raw[5]<=CINEMA_BOOKMARK_MAX&&r32(raw+8)==key){c=r32(raw+40);if(c==cin2_crc32(raw,40)){b->count=raw[5];for(i=0;i<b->count;i++)b->frame[i]=r32(raw+12+i*4);}}ti_Close(h);}
void cinema_bookmarks_save(const cinema_bookmarks_t*b){uint8_t h,raw[REC_BYTES]={0},i;memcpy(raw,"C2BM",4);raw[4]=1;raw[5]=b->count;w32(raw+8,b->movie_key);for(i=0;i<b->count;i++)w32(raw+12+i*4,b->frame[i]);w32(raw+40,cin2_crc32(raw,40));h=ti_Open(CINEMA_BOOKMARK_APPVAR,"w");if(h){ti_Write(raw,1,REC_BYTES,h);ti_Close(h);}}
void cinema_bookmarks_add(cinema_bookmarks_t*b,uint32_t f){uint8_t i;for(i=0;i<b->count;i++)if(b->frame[i]==f)return;if(b->count<CINEMA_BOOKMARK_MAX)b->frame[b->count++]=f;else{for(i=1;i<CINEMA_BOOKMARK_MAX;i++)b->frame[i-1]=b->frame[i];b->frame[CINEMA_BOOKMARK_MAX-1]=f;}cinema_bookmarks_save(b);}

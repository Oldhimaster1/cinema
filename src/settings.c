#include "settings.h"
#include "cin2.h"
#include <fileioc.h>
#include <string.h>
static const uint16_t seeks[]={5,10,30,60,300};
void cinema_settings_defaults(cinema_settings_t *s){s->seek_index=1;s->dashboard_seconds=3;s->scale_mode=0;s->auto_resume=0;}
void cinema_settings_load(cinema_settings_t *s){uint8_t h,raw[12];cinema_settings_defaults(s);h=ti_Open(CINEMA_SETTINGS_APPVAR,"r");if(!h)return;if(ti_Read(raw,1,12,h)==12&&memcmp(raw,"C2ST",4)==0&&raw[4]==1&&cin2_crc32(raw,8)==((uint32_t)raw[8]|((uint32_t)raw[9]<<8)|((uint32_t)raw[10]<<16)|((uint32_t)raw[11]<<24))){s->seek_index=raw[5]<5?raw[5]:1;s->dashboard_seconds=raw[6]?raw[6]:3;s->scale_mode=raw[7]<3?raw[7]:0;}ti_Close(h);}
void cinema_settings_save(const cinema_settings_t *s){uint8_t h,raw[12]={0};uint32_t c;memcpy(raw,"C2ST",4);raw[4]=1;raw[5]=s->seek_index;raw[6]=s->dashboard_seconds;raw[7]=s->scale_mode;c=cin2_crc32(raw,8);raw[8]=c;raw[9]=c>>8;raw[10]=c>>16;raw[11]=c>>24;h=ti_Open(CINEMA_SETTINGS_APPVAR,"w");if(h){ti_Write(raw,1,12,h);ti_Close(h);}}
uint16_t cinema_seek_seconds(const cinema_settings_t *s){return seeks[s->seek_index<5?s->seek_index:1];}

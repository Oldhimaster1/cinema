#include "cinema_settings.h"
#include "cinema_codec.h"
#include <string.h>
#define VALID 15u
void cinema_settings_defaults(cinema_settings_t*s){s->flags=CINEMA_SETTING_STATUS|CINEMA_SETTING_THUMBS|CINEMA_SETTING_REMEMBER;}
void cinema_settings_encode(uint8_t*b,const cinema_settings_t*s){memset(b,0,CINEMA_SETTINGS_BYTES);memcpy(b,"CST1",4);b[4]=1;b[5]=s->flags&VALID;cinema_wr32(b+12,cinema_crc32_zeroed(b,CINEMA_SETTINGS_BYTES,12,4));}
bool cinema_settings_decode(cinema_settings_t*s,const uint8_t*b,size_t n){cinema_settings_defaults(s);if(n!=CINEMA_SETTINGS_BYTES||memcmp(b,"CST1",4)||b[4]!=1||(b[5]&~VALID)||cinema_rd32(b+12)!=cinema_crc32_zeroed(b,n,12,4))return false;s->flags=b[5];return true;}

bool cinema_settings_equal(const cinema_settings_t*a,const cinema_settings_t*b){return a&&b&&a->flags==b->flags;}

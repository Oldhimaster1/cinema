#include "../src/settings.h"
#include "../src/bookmarks.h"
#include <stdio.h>
#include <string.h>
static int fail;
#define C(x,m) do{if(!(x)){printf("FAIL %s\n",m);fail++;}}while(0)
int main(void){cinema_settings_t a,b;cinema_bookmarks_t x,y;cin2_header_t h;uint32_t key;memset(&h,0,sizeof(h));h.width=160;h.height=96;h.fps_num=24;h.fps_den=1;h.frame_count=1000;cinema_settings_defaults(&a);a.seek_index=4;a.dashboard_seconds=7;a.scale_mode=2;cinema_settings_save(&a);cinema_settings_load(&b);C(b.seek_index==4&&b.dashboard_seconds==7&&b.scale_mode==2,"settings roundtrip");key=cinema_movie_key(&h);memset(&x,0,sizeof(x));x.movie_key=key;cinema_bookmarks_add(&x,100);cinema_bookmarks_add(&x,200);cinema_bookmarks_load(&y,key);C(y.count==2&&y.frame[0]==100&&y.frame[1]==200,"bookmarks roundtrip");if(fail)return 1;puts("All persistence tests passed.");return 0;}

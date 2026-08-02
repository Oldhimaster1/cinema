#include "../src/cin2.h"
#include "../src/decode.h"
#include "../src/ui.h"
#include <stdio.h>
#include <string.h>
static int fail;
#define C(x,m) do { if (!(x)) { printf("FAIL %s\n",m); fail++; } } while (0)
int main(void)
{
    uint8_t raw[512], packed[CINEMA_V2_PACKED_BYTES];
    static uint8_t fb[320*240];
    cin2_header_t a,b;
    unsigned i;
    memset(&a,0,sizeof(a));
    a.width=160; a.height=96; a.fps_num=24000; a.fps_den=1001; a.frame_count=10000;
    strcpy(a.title,"TEST MOVIE");
    a.chapter_count=2;
    a.chapters[0].frame=0; strcpy(a.chapters[0].name,"OPENING");
    a.chapters[1].frame=5000; strcpy(a.chapters[1].name,"MIDDLE");
    cin2_build_header(raw,&a);
    C(cin2_parse_header(raw,&b),"metadata parse");
    C(!strcmp(b.title,"TEST MOVIE"),"title");
    C(b.chapter_count==2 && b.chapters[1].frame==5000,"chapters");
    raw[402]^=1;
    C(!cin2_parse_header(raw,&b),"metadata crc");
    for(i=0;i<sizeof(packed);i++) packed[i]=(uint8_t)i;
    memset(fb,0xaa,sizeof(fb));
    cinema_draw_packed4_original(packed,fb,320);
    C(fb[0]==0xaa && fb[72*320+80]==0,"original bounds");
    memset(fb,0,sizeof(fb));
    cinema_draw_packed4_stretch(packed,fb,320);
    C(fb[0]==0 && fb[319]==15,"stretch first row");
    ui_draw_text(fb,320,0,0,"CINEMA 24",15);
    ui_draw_progress(fb,320,50,100,3,4,0,24,1);
    if(fail) return 1;
    puts("All feature tests passed.");
    return 0;
}

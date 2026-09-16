#include "../src/cinema_subtitle.h"
#include "../src/cinema_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t n; unsigned reads; } mem_t;
static bool rd(void *v,uint32_t o,uint8_t *d,size_t n)
{
    mem_t *m=v;
    if((uint64_t)o+n>m->n)return false;
    memcpy(d,m->p+o,n);m->reads++;
    return true;
}
static void wr32(uint8_t *p,uint32_t v){cinema_wr32(p,v);}

int main(void)
{
    const uint32_t count=1000,frames=5000,id=0x12345678u;
    size_t n=40u+72u*count,i;
    uint8_t *b=calloc(1,n);
    mem_t m;
    csu_header_t h;
    csu_cue_t c;
    int32_t idx;
    unsigned expected_validation_reads=1u+(unsigned)((n+511u)/512u);
    if(!b)return 2;
    memcpy(b,"CSU1",4);b[4]=1;cinema_wr16(b+6,40);cinema_wr16(b+8,72);
    wr32(b+12,count);wr32(b+16,id);wr32(b+20,frames);wr32(b+24,20);wr32(b+28,1);
    for(i=0;i<count;i++){
        uint8_t *r=b+40+i*72;
        wr32(r,(uint32_t)i*4);wr32(r+4,(uint32_t)i*4+3);
        r[8]=1;r[9]=1;r[12]='X';
    }
    wr32(b+32,cinema_crc32_zeroed(b,n,32,4));
    m.p=b;m.n=n;m.reads=0;
    if(n<=65536||!csu_stream_validate(&h,rd,&m,(uint32_t)n,id,frames,20,1))return 1;
    if(m.reads!=expected_validation_reads){
        fprintf(stderr,"validation read regression: got %u expected %u\n",m.reads,expected_validation_reads);
        return 1;
    }
    idx=csu_stream_find_cue(&h,rd,&m,321);
    if(idx!=80||!csu_stream_read_cue(&c,rd,&m,(uint32_t)idx)||c.line1[0]!='X')return 1;
    idx=csu_stream_find_at_or_after(&h,rd,&m,323);
    if(idx!=81||!csu_stream_read_cue(&c,rd,&m,(uint32_t)idx)
       ||c.start_frame!=324||c.end_frame!=327)return 1;
    if(csu_stream_find_at_or_after(&h,rd,&m,4999)!=-1)return 1;
    b[70000]^=1;m.reads=0;
    if(csu_stream_validate(&h,rd,&m,(uint32_t)n,id,frames,20,1))return 1;
    free(b);
    puts("streamed CSU validation, bounded I/O, and gap lookup tests passed.");
    return 0;
}

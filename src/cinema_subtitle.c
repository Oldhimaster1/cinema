#include "cinema_subtitle.h"
#include "cinema_codec.h"
#include <string.h>
bool csu_parse_header(csu_header_t*h,const uint8_t*b,size_t n){if(n<CSU_HEADER_SIZE||memcmp(b,"CSU1",4)||b[4]!=1||cinema_rd16(b+6)!=CSU_HEADER_SIZE||cinema_rd16(b+8)!=CSU_RECORD_SIZE)return false;h->cue_count=cinema_rd32(b+12);h->movie_id=cinema_rd32(b+16);h->frame_count=cinema_rd32(b+20);h->fps_num=cinema_rd32(b+24);h->fps_den=cinema_rd32(b+28);h->file_crc=cinema_rd32(b+32);return h->fps_num&&h->fps_den;}
bool csu_parse_cue(csu_cue_t*c,const uint8_t*b,size_t n){if(n<CSU_RECORD_SIZE)return false;c->start_frame=cinema_rd32(b);c->end_frame=cinema_rd32(b+4);c->line_count=b[8];c->line1_len=b[9];c->line2_len=b[10];if(c->start_frame>=c->end_frame||c->line_count<1||c->line_count>2||c->line1_len>CSU_LINE_MAX||c->line2_len>CSU_LINE_MAX||(c->line_count==1&&c->line2_len))return false;memcpy(c->line1,b+12,c->line1_len);c->line1[c->line1_len]=0;memcpy(c->line2,b+42,c->line2_len);c->line2[c->line2_len]=0;return true;}
bool csu_validate_file(const uint8_t*b,size_t n,uint32_t id,uint32_t frames,uint32_t fn,uint32_t fd){csu_header_t h;uint32_t i,prev=0;if(!csu_parse_header(&h,b,n)||h.movie_id!=id||h.frame_count!=frames||h.fps_num!=fn||h.fps_den!=fd||n!=CSU_HEADER_SIZE+(size_t)h.cue_count*CSU_RECORD_SIZE||h.file_crc!=cinema_crc32_zeroed(b,n,32,4))return false;for(i=0;i<h.cue_count;i++){csu_cue_t c;if(!csu_parse_cue(&c,b+CSU_HEADER_SIZE+(size_t)i*CSU_RECORD_SIZE,CSU_RECORD_SIZE)||c.end_frame>frames||(i&&c.start_frame<prev))return false;prev=c.end_frame;}return true;}
int32_t csu_find_cue(const uint8_t*b,size_t n,const csu_header_t*h,uint32_t f){uint32_t lo=0,hi=h->cue_count;if(n!=CSU_HEADER_SIZE+(size_t)h->cue_count*CSU_RECORD_SIZE)return -1;while(lo<hi){uint32_t m=lo+(hi-lo)/2;uint32_t s=cinema_rd32(b+CSU_HEADER_SIZE+(size_t)m*CSU_RECORD_SIZE);if(s<=f)lo=m+1;else hi=m;}if(!lo)return -1;lo--;return f<cinema_rd32(b+CSU_HEADER_SIZE+(size_t)lo*CSU_RECORD_SIZE+4)?(int32_t)lo:-1;}


static uint32_t csu_crc_step(uint32_t c,const uint8_t *p,size_t n,uint32_t absolute)
{
    size_t i; unsigned k;
    for(i=0;i<n;i++,absolute++){
        uint8_t b=(absolute>=32u&&absolute<36u)?0:p[i];
        c^=b; for(k=0;k<8;k++) c=(c>>1)^(0xedb88320u&(uint32_t)-(int32_t)(c&1u));
    }
    return c;
}
bool csu_stream_validate(csu_header_t *out,csu_stream_read_t read_cb,void *ctx,
                         uint32_t file_size,uint32_t movie_id,uint32_t frames,
                         uint32_t fps_num,uint32_t fps_den)
{
    uint8_t buf[512], record[CSU_RECORD_SIZE];
    csu_header_t h;
    uint32_t off=0,crc=0xffffffffu,prev=0,cue_index=0;
    size_t record_used=0;
    if(!out||!read_cb||file_size<CSU_HEADER_SIZE
       ||!read_cb(ctx,0,buf,CSU_HEADER_SIZE)
       ||!csu_parse_header(&h,buf,CSU_HEADER_SIZE)
       ||(movie_id!=0u&&h.movie_id!=movie_id)
       ||h.frame_count!=frames||h.fps_num!=fps_num||h.fps_den!=fps_den
       ||(uint64_t)CSU_HEADER_SIZE+(uint64_t)h.cue_count*CSU_RECORD_SIZE!=file_size)
        return false;

    /* Validate CRC and cue ordering in one sequential pass.  The previous
     * implementation first scanned the complete file for CRC, then issued
     * one streamed read per cue.  On a USB-backed one-sector cache that made
     * opening a 1,958-cue sidecar require thousands of serialized commands. */
    while(off<file_size){
        size_t n=file_size-off,pos=0;
        if(n>sizeof(buf))n=sizeof(buf);
        if(!read_cb(ctx,off,buf,n))return false;
        crc=csu_crc_step(crc,buf,n,off);
        if(off<CSU_HEADER_SIZE){
            size_t skip=CSU_HEADER_SIZE-off;
            if(skip>n)skip=n;
            pos=skip;
        }
        while(pos<n){
            size_t take=CSU_RECORD_SIZE-record_used;
            csu_cue_t cue;
            if(take>n-pos)take=n-pos;
            memcpy(record+record_used,buf+pos,take);
            record_used+=take;pos+=take;
            if(record_used==CSU_RECORD_SIZE){
                if(cue_index>=h.cue_count
                   ||!csu_parse_cue(&cue,record,sizeof(record))
                   ||cue.end_frame>frames
                   ||(cue_index&&cue.start_frame<prev))return false;
                prev=cue.end_frame;cue_index++;record_used=0;
            }
        }
        off+=(uint32_t)n;
    }
    if(record_used!=0||cue_index!=h.cue_count||(~crc)!=h.file_crc)return false;
    *out=h;return true;
}

bool csu_stream_open_fast(csu_header_t *out,csu_stream_read_t read_cb,void *ctx,
                          uint32_t file_size,uint32_t movie_id,uint32_t frames,
                          uint32_t fps_num,uint32_t fps_den)
{
    uint8_t raw_header[CSU_HEADER_SIZE];
    csu_header_t h;
    uint64_t expected_size;
    if(!out||!read_cb||file_size<CSU_HEADER_SIZE
       ||!read_cb(ctx,0,raw_header,sizeof(raw_header))
       ||!csu_parse_header(&h,raw_header,sizeof(raw_header))
       ||(movie_id!=0u&&h.movie_id!=movie_id)
       ||h.frame_count!=frames||h.fps_num!=fps_num||h.fps_den!=fps_den) return false;
    expected_size=(uint64_t)CSU_HEADER_SIZE+(uint64_t)h.cue_count*CSU_RECORD_SIZE;
    if(expected_size!=file_size) return false;
    if(h.cue_count!=0u){
        csu_cue_t first;
        if(!csu_stream_read_cue(&first,read_cb,ctx,0u)||first.end_frame>frames) return false;
    }
    *out=h;
    return true;
}

int32_t csu_stream_find_cue(const csu_header_t *h,csu_stream_read_t read_cb,void *ctx,uint32_t frame)
{
    uint32_t lo=0,hi=h?h->cue_count:0; uint8_t b[8];
    if(!h||!read_cb)return -2;
    while(lo<hi){uint32_t m=lo+(hi-lo)/2;
        if(!read_cb(ctx,CSU_HEADER_SIZE+m*CSU_RECORD_SIZE,b,8))return -2;
        if(cinema_rd32(b)<=frame)lo=m+1;else hi=m;}
    if(!lo)return -1;
    lo--;
    if(!read_cb(ctx,CSU_HEADER_SIZE+lo*CSU_RECORD_SIZE,b,8))return -2;
    return frame<cinema_rd32(b+4)?(int32_t)lo:-1;
}

int32_t csu_stream_find_at_or_after(const csu_header_t *h,
                                    csu_stream_read_t read_cb,void *ctx,
                                    uint32_t frame)
{
    uint32_t lo=0,hi=h?h->cue_count:0;uint8_t b[8];
    if(!h||!read_cb)return -2;
    /* First cue whose end is after FRAME.  The result is either active now
     * or is the next upcoming cue, so callers can cache subtitle-free gaps. */
    while(lo<hi){uint32_t m=lo+(hi-lo)/2;
        if(!read_cb(ctx,CSU_HEADER_SIZE+m*CSU_RECORD_SIZE,b,8))return -2;
        if(cinema_rd32(b+4)<=frame)lo=m+1;else hi=m;
    }
    return lo<h->cue_count?(int32_t)lo:-1;
}
bool csu_stream_read_cue(csu_cue_t *out,csu_stream_read_t read_cb,void *ctx,uint32_t index)
{
    uint8_t b[CSU_RECORD_SIZE];
    return out&&read_cb&&read_cb(ctx,CSU_HEADER_SIZE+index*CSU_RECORD_SIZE,b,sizeof(b))
        &&csu_parse_cue(out,b,sizeof(b));
}

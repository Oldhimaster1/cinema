#ifndef CINEMA_SUBTITLE_H
#define CINEMA_SUBTITLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CSU_HEADER_SIZE 40u
#define CSU_LINE_MAX 30u
#define CSU_RECORD_SIZE 72u
typedef struct{uint32_t movie_id,frame_count,fps_num,fps_den,cue_count,file_crc;}csu_header_t;
typedef struct{uint32_t start_frame,end_frame;uint8_t line_count,line1_len,line2_len;char line1[CSU_LINE_MAX+1],line2[CSU_LINE_MAX+1];}csu_cue_t;
bool csu_parse_header(csu_header_t*h,const uint8_t*b,size_t n);
bool csu_parse_cue(csu_cue_t*c,const uint8_t*b,size_t n);
bool csu_validate_file(const uint8_t*b,size_t n,uint32_t movie_id,uint32_t frames,uint32_t fn,uint32_t fd);
int32_t csu_find_cue(const uint8_t*b,size_t n,const csu_header_t*h,uint32_t frame);
typedef bool (*csu_stream_read_t)(void *ctx,uint32_t offset,uint8_t *out,size_t size);
bool csu_stream_validate(csu_header_t *out,csu_stream_read_t read_cb,void *ctx,
                         uint32_t file_size,uint32_t movie_id,uint32_t frames,
                         uint32_t fps_num,uint32_t fps_den);
/* Bounded startup validation: reads only the CSU1 header and, when present,
 * the first cue. It validates structure, exact file size, movie binding, and
 * first-cue bounds. The full-file CRC/order pass remains csu_stream_validate(). */
bool csu_stream_open_fast(csu_header_t *out,csu_stream_read_t read_cb,void *ctx,
                          uint32_t file_size,uint32_t movie_id,uint32_t frames,
                          uint32_t fps_num,uint32_t fps_den);
int32_t csu_stream_find_cue(const csu_header_t *h,csu_stream_read_t read_cb,
                            void *ctx,uint32_t frame);
int32_t csu_stream_find_at_or_after(const csu_header_t *h,csu_stream_read_t read_cb,
                                    void *ctx,uint32_t frame);
bool csu_stream_read_cue(csu_cue_t *out,csu_stream_read_t read_cb,void *ctx,uint32_t index);
#endif

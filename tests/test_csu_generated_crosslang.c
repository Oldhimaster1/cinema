/* Verifies a real encoder-generated CIN2 + CSU pair with the calculator's
 * production C parsers and streaming cue lookup, not a Python mirror. */
#include "../src/cin2.h"
#include "../src/cinema_subtitle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t length;
} memory_stream_t;

static bool read_memory(void *opaque, uint32_t offset, uint8_t *out, size_t size)
{
    const memory_stream_t *s = (const memory_stream_t *)opaque;
    if ((uint64_t)offset + size > s->length) return false;
    memcpy(out, s->bytes + offset, size);
    return true;
}

int main(int argc, char **argv)
{
    uint8_t movie_header[CIN2_HEADER_BYTES];
    cin2_header_t movie;
    csu_header_t subtitle;
    csu_cue_t first, second;
    memory_stream_t stream;
    FILE *f;
    long file_size;
    int rc = 1;

    if (argc != 3) return 2;
    f = fopen(argv[1], "rb");
    if (!f) return 3;
    if (fread(movie_header, 1, sizeof(movie_header), f) != sizeof(movie_header)) {
        fclose(f); return 4;
    }
    fclose(f);
    if (!cin2_parse_header(movie_header, &movie)) return 5;

    f = fopen(argv[2], "rb");
    if (!f) return 6;
    if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 0
        || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 7; }
    stream.length = (size_t)file_size;
    stream.bytes = malloc(stream.length);
    if (!stream.bytes) { fclose(f); return 8; }
    if (fread((void *)stream.bytes, 1, stream.length, f) != stream.length) {
        fclose(f); free((void *)stream.bytes); return 9;
    }
    fclose(f);

    if (!csu_stream_open_fast(&subtitle, read_memory, &stream, (uint32_t)stream.length,
                              0u, movie.frame_count, movie.fps_num, movie.fps_den))
        goto done;
    if (!csu_stream_validate(&subtitle, read_memory, &stream, (uint32_t)stream.length,
                             0u, movie.frame_count, movie.fps_num, movie.fps_den))
        goto done;
    if (subtitle.cue_count != 2u || !csu_stream_read_cue(&first, read_memory, &stream, 0u)
        || !csu_stream_read_cue(&second, read_memory, &stream, 1u))
        goto done;
    if (strcmp(first.line1, "Hello Cinema!") != 0
        || strcmp(second.line1, "Subtitles still work.") != 0)
        goto done;
    if (csu_stream_find_at_or_after(&subtitle, read_memory, &stream, 0u) != 0
        || csu_stream_find_at_or_after(&subtitle, read_memory, &stream, second.start_frame) != 1)
        goto done;

    rc = 0;
    puts("PASS: generated subtitles accepted by production C streamer");
done:
    free((void *)stream.bytes);
    return rc;
}

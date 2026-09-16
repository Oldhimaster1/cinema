#include "placement_map.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(x,m) do { if (!(x)) { printf("FAIL: %s\n",m); fails++; } } while (0)
static void make_case(uint8_t *raw, fat32ro_volume_t *v, fat32ro_dirent_t *e)
{
    cin2_header_t h = {160,96,20,1,2,{0}};
    cin2_placement_t p;
    memset(v,0,sizeof(*v)); memset(e,0,sizeof(*e)); memset(&p,0,sizeof(p));
    v->sectors_per_cluster=4; v->volume_serial=0x12345678;
    v->partition_base_lba=0; v->first_fat_sector=32; v->first_data_sector=100;
    v->fat_size_sectors=64; v->total_clusters=10000;
    strcpy(e->name,"MOVIE.BIN"); e->first_cluster=4; e->file_size=60u*512u;
    cin2_build_header(raw,&h);
    p.flags=CIN2_PLACEMENT_FLAG_PREPARED; p.extent_count=2;
    p.logical_sector_bytes=512; p.sectors_per_cluster=4;
    p.volume_serial=v->volume_serial; p.partition_base_lba=v->partition_base_lba;
    p.first_fat_lba=v->first_fat_sector; p.first_data_lba=v->first_data_sector;
    p.fat_size_sectors=v->fat_size_sectors; p.total_data_clusters=v->total_clusters;
    p.movie_first_cluster=e->first_cluster; p.movie_file_size=e->file_size;
    p.total_movie_sectors=60; p.immutable_header_crc=cin2_crc32(raw,CIN2_CRC_BYTES);
    p.extents[0].start_lba=108; p.extents[0].sector_count=20;
    p.extents[1].start_lba=500; p.extents[1].sector_count=40;
    CHECK(cin2_build_placement(raw,&p),"build fixture");
}
int main(void)
{
    uint8_t raw[CIN2_HEADER_BYTES]; fat32ro_volume_t v; fat32ro_dirent_t e;
    fat32ro_extent_map_t out, sentinel;
    make_case(raw,&v,&e); memset(&out,0,sizeof(out));
    CHECK(placement_map_from_header(raw,&v,&e,&out),"valid prepared map accepted");
    CHECK(out.extent_count==2 && out.total_sectors==60,"map metadata copied");
    CHECK(out.extents[0].lba==108 && out.extents[1].lba==500,"extents copied");
    sentinel=out; v.volume_serial++;
    CHECK(!placement_map_from_header(raw,&v,&e,&out),"wrong volume rejected");
    CHECK(memcmp(&out,&sentinel,sizeof(out))==0,"rejection leaves output unchanged");
    v.volume_serial--; e.first_cluster++;
    CHECK(!placement_map_from_header(raw,&v,&e,&out),"wrong first cluster rejected");
    e.first_cluster--; raw[CIN2_PLACEMENT_EXTENTS_OFFSET]^=1;
    CHECK(!placement_map_from_header(raw,&v,&e,&out),"corrupt extent rejected");
    cin2_build_header(raw,&(cin2_header_t){160,96,20,1,2,{0}});
    CHECK(!placement_map_from_header(raw,&v,&e,&out),"missing placement rejected");
    if (!fails) { puts("All placement-map tests passed."); return 0; }
    return 1;
}

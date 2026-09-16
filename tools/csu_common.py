import struct,zlib
HS=40;RS=72;LM=30
HDR=struct.Struct('<4sBBHHHIIIIII')
def crc(data):
 b=bytearray(data);b[32:36]=b'\0'*4;return zlib.crc32(b)&0xffffffff
def movie_id(name,frame_count,fps_num,fps_den,file_size):
 x=zlib.crc32(name.upper().encode('ascii','replace'))&0xffffffff
 return zlib.crc32(struct.pack('<IIII',frame_count,fps_num,fps_den,file_size),x)&0xffffffff

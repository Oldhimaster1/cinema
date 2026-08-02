#!/usr/bin/env python3
import argparse,struct,zlib
from pathlib import Path
OFF=58; MAX_CH=12; TITLE=48; REC=24
ap=argparse.ArgumentParser();ap.add_argument('image');ap.add_argument('--title',default='');ap.add_argument('--chapter',action='append',default=[],help='FRAME:NAME');ns=ap.parse_args()
p=Path(ns.image);b=bytearray(p.read_bytes())
if len(b)<512 or b[:4]!=b'CIN2': raise SystemExit('not CIN2')
frames=struct.unpack_from('<I',b,18)[0]
meta=bytearray(348);meta[:4]=b'C2MD';meta[4]=1
t=ns.title.encode('ascii','replace')[:TITLE];meta[5]=len(t);chs=[]
for item in ns.chapter:
 f,n=item.split(':',1);f=int(f);n=n.encode('ascii','replace')[:19]
 if not 0 <= f < frames: raise SystemExit('chapter frame out of range')
 chs.append((f,n))
if len(chs)>MAX_CH: raise SystemExit('maximum 12 chapters')
meta[6]=len(chs);meta[8:8+len(t)]=t
for i,(f,n) in enumerate(chs):
 o=56+i*REC;struct.pack_into('<I',meta,o,f);meta[o+4]=len(n);meta[o+5:o+5+len(n)]=n
struct.pack_into('<I',meta,344,zlib.crc32(meta[:344])&0xffffffff)
b[OFF:OFF+348]=meta;p.write_bytes(b);print(p)

#!/usr/bin/env python3
import argparse,struct
from pathlib import Path
from csu_common import *
def verify(p):
 b=Path(p).read_bytes()
 if len(b)<HS:raise ValueError('short header')
 magic,ver,flags,hs,rs,res,count,mid,frames,fn,fd,got=HDR.unpack(b[:36])
 if (magic,ver,flags,hs,rs,res)!=(b'CSU1',1,0,HS,RS,0):raise ValueError('bad header')
 if not fn or not fd or len(b)!=HS+count*RS or got!=crc(b):raise ValueError('size/fps/crc')
 prev=0
 for i in range(count):
  r=b[HS+i*RS:HS+(i+1)*RS];s,e,lc,a,z,fl=struct.unpack('<IIBBBB',r[:12])
  if s>=e or e>frames or s<prev or lc not in (1,2) or a>LM or z>LM or (lc==1 and z) or fl:raise ValueError(f'cue {i}')
  if any(r[12+a:42]) or any(r[42+z:72]):raise ValueError(f'padding {i}')
  prev=e
 return count,mid
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('file');a=ap.parse_args();c,m=verify(a.file);print(f'PASS cues={c} movie_id={m:08X}')

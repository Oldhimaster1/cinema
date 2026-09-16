#!/usr/bin/env python3
"""Convert SRT subtitles to Cinema CSU1 with explicit compatibility reports."""
import argparse,json,re,struct,unicodedata
from pathlib import Path
from csu_common import *
TS=re.compile(r'^(\d+):(\d\d):(\d\d)[,.](\d{3})\s*-->\s*(\d+):(\d\d):(\d\d)[,.](\d{3})$')
REPLACEMENTS={'\u2018':"'",'\u2019':"'",'\u201c':'"','\u201d':'"','\u2013':'-','\u2014':'-','\u2026':'...','\u00a0':' '}
def ms(v):h,m,s,z=map(int,v);return ((h*60+m)*60+s)*1000+z
def frame(t,n,d):return (t*n+500*d)//(1000*d)
def normalize_text(s,cue,report):
 s=re.sub(r'<[^>]*>|\{\\[^}]*\}','',s)
 for old,new in REPLACEMENTS.items():
  if old in s:
   count=s.count(old);report['replacement_count']+=count
   report['replacements'].append({'cue':cue,'codepoint':f'U+{ord(old):04X}','count':count,'replacement':new})
   s=s.replace(old,new)
 out=[]
 for ch in unicodedata.normalize('NFKD',s):
  if unicodedata.combining(ch):report['replacement_count']+=1;continue
  if ord(ch)<128:out.append(ch)
  else:
   report['replacement_count']+=1;report['replacements'].append({'cue':cue,'codepoint':f'U+{ord(ch):04X}','count':1,'replacement':'?'})
   out.append('?')
 return ' '.join(''.join(out).split())
def wrap_balanced(s):
 words=s.split()
 if not words:return ['']
 if any(len(w)>LM for w in words):raise ValueError('word exceeds 30-character calculator line limit')
 if len(s)<=LM:return [s]
 candidates=[]
 for i in range(1,len(words)):
  a=' '.join(words[:i]);b=' '.join(words[i:])
  if len(a)<=LM and len(b)<=LM and not re.match(r'^[,.;:!?]',b):
   orphan=8 if len(b.split()[0])<=2 else 0
   candidates.append((abs(len(a)-len(b))+orphan,-len(a),a,b))
 if not candidates:raise ValueError('cue cannot fit in two 30-character lines')
 _,_,a,b=min(candidates);return [a,b]
def parse(t,report):
 text=re.sub(r'\r+\n','\n',t.lstrip('\ufeff')).replace('\r','\n').strip()
 blocks=re.split(r'\n[ \t]*\n',text) if text else []
 out=[]
 for number,b in enumerate(blocks,1):
  q=b.splitlines()
  if q and q[0].strip().isdigit():q=q[1:]
  if len(q)<2:raise ValueError(f'cue {number}: incomplete cue')
  m=TS.match(q[0].strip())
  if not m:raise ValueError(f'cue {number}: bad timestamp')
  a=ms(m.groups()[:4]);z=ms(m.groups()[4:])
  if z<=a:raise ValueError(f'cue {number}: non-positive duration')
  cleaned=normalize_text(' '.join(q[1:]),number,report);lines=wrap_balanced(cleaned)
  if len(lines)==2:report['wrapped_cues']+=1
  report['longest_line']=max([report['longest_line']]+[len(x) for x in lines])
  out.append((a,z,lines))
 report['input_cues']=len(out);return out
def convert(srt,output,name,frames,fps_num,fps_den,movie_size,report_path=None):
 report={'input':str(srt),'output':str(output),'input_cues':0,'output_cues':0,'one_line_cues':0,'two_line_cues':0,'wrapped_cues':0,'replacement_count':0,'replacements':[],'longest_line':0,'warnings':[],'result':'FAIL'}
 cues=parse(Path(srt).read_text(encoding='utf-8-sig'),report);rec=[];prev=0
 for i,(x,y,lines) in enumerate(cues,1):
  start,end=frame(x,fps_num,fps_den),min(frames,frame(y,fps_num,fps_den))
  if start<prev:raise ValueError(f'cue {i}: overlaps preceding cue')
  if end<=start:raise ValueError(f'cue {i}: collapses after frame conversion')
  l1=lines[0].encode('ascii');l2=(lines[1] if len(lines)>1 else '').encode('ascii')
  rec.append(struct.pack('<IIBBBB',start,end,len(lines),len(l1),len(l2),0)+l1.ljust(LM,b'\0')+l2.ljust(LM,b'\0'));prev=end
  report['two_line_cues' if len(lines)==2 else 'one_line_cues']+=1
 mid=movie_id(name,frames,fps_num,fps_den,movie_size)
 h=HDR.pack(b'CSU1',1,0,HS,RS,0,len(rec),mid,frames,fps_num,fps_den,0)+b'\0'*4
 data=h+b''.join(rec);data=data[:32]+struct.pack('<I',crc(data))+data[36:]
 tmp=Path(str(output)+'.partial');tmp.write_bytes(data)
 try:
  from verify_csu import verify
  count,verified_mid=verify(tmp)
  if count!=len(rec) or verified_mid!=mid:raise ValueError('independent CSU verification mismatch')
  tmp.replace(output)
 except BaseException:
  tmp.unlink(missing_ok=True);raise
 report.update(output_cues=len(rec),movie_id=f'{mid:08X}',result='PASS')
 rp=Path(report_path) if report_path else Path(str(output)+'.report.json')
 rp.write_text(json.dumps(report,indent=2),encoding='utf-8')
 print(f"PASS cues={len(rec)} movie_id={mid:08X} replacements={report['replacement_count']} wrapped={report['wrapped_cues']}")
 print(f'report: {rp}')
 return report
def main():
 ap=argparse.ArgumentParser();ap.add_argument('srt',type=Path);ap.add_argument('output',type=Path);ap.add_argument('--name',required=True);ap.add_argument('--frames',type=int,required=True);ap.add_argument('--fps-num',type=int,required=True);ap.add_argument('--fps-den',type=int,default=1);ap.add_argument('--movie-size',type=int,required=True);ap.add_argument('--report',type=Path);a=ap.parse_args()
 convert(a.srt,a.output,a.name,a.frames,a.fps_num,a.fps_den,a.movie_size,a.report)
if __name__=='__main__':main()

import json,sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
import srt_to_csu as s
from verify_csu import verify

def test_balanced_wrap_and_unicode_report(tmp_path):
 p=tmp_path/'a.srt';o=tmp_path/'M.csu'
 p.write_text('1\n00:00:01,000 --> 00:00:03,000\n“Hello”—this is a balanced subtitle line…\n',encoding='utf-8')
 r=s.convert(p,o,'M.bin',100,20,1,999)
 assert r['result']=='PASS' and r['replacement_count']>=3 and r['wrapped_cues']==1
 assert verify(o)[0]==1 and json.loads(Path(str(o)+'.report.json').read_text())['result']=='PASS'
def test_unfit_cue_rejected_without_output(tmp_path):
 p=tmp_path/'a.srt';o=tmp_path/'M.csu';p.write_text('1\n00:00:00,000 --> 00:00:02,000\n'+'word '*40,encoding='utf-8')
 with pytest.raises(ValueError):s.convert(p,o,'M.bin',100,20,1,999)
 assert not o.exists() and not Path(str(o)+'.partial').exists()
def test_overlap_rejected(tmp_path):
 p=tmp_path/'a.srt';o=tmp_path/'M.csu';p.write_text('1\n00:00:00,000 --> 00:00:02,000\nA\n\n2\n00:00:01,000 --> 00:00:03,000\nB\n',encoding='utf-8')
 with pytest.raises(ValueError,match='overlaps'):s.convert(p,o,'M.bin',100,20,1,999)

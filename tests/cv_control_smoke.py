"""Board integration: toggles shared detection mode, restores it on exit.
python tests/cv_control_smoke.py http://127.0.0.1:18080 report.json
"""
import json
from pathlib import Path
import sys
import time
from urllib.request import Request, urlopen

base=sys.argv[1].rstrip('/')
def get(path):
    with urlopen(base+path,timeout=5) as r:return json.load(r)
def set_mode(mode):
    name=['off','person','objects','all'][mode]
    with urlopen(Request(base+'/api/detection/'+name,data=b'',method='POST'),timeout=5) as r:
        assert json.load(r)['ok']
def ready(mode):
    deadline=time.monotonic()+12
    while time.monotonic()<deadline:
        s=get('/api/status');i=s['inference'];r=i.get('result',{})
        if i.get('mode')==mode and (not mode and r.get('state')=='disabled' or
           mode and i['available'] and r.get('mode')==mode and r['frames']>=3):
            assert s['streams']['flv']['ready']
            assert s['performance']['encoded_fps']>10
            return s
        time.sleep(.25)
    raise AssertionError(s)
original=get('/api/status')['inference'].get('mode',0)
report={}
try:
    set_mode(1);s=ready(1);report['person']=s['inference']
    def exact(stream,n):
        data=stream.read(n);assert len(data)==n;return data
    frames=[];pending=None
    with urlopen(base+'/live.flv',timeout=5) as stream:
        assert exact(stream,13)[:3]==b'FLV'
        while len(frames)<25:
            header=exact(stream,11);size=int.from_bytes(header[1:4],'big')
            assert size<2*1024*1024
            payload=exact(stream,size);exact(stream,4)
            pts=int.from_bytes(header[4:7],'big')+(header[7]<<24)
            if header[0]==18:
                assert payload[0]==2
                n=int.from_bytes(payload[1:3],'big');name=payload[3:3+n]
                if name==b'onFrameClock':
                    offset=3+n;assert payload[offset]==2
                    n=int.from_bytes(payload[offset+1:offset+3],'big')
                    pending=json.loads(payload[offset+3:offset+3+n])
                    assert pending['pts_ms']==pts
            if header[0]==9 and len(payload)>1 and payload[1]==1:
                assert pending and pending['pts_ms']==pts
                frames.append(pending);pending=None
    assert frames[0]['pts_ms']==0
    assert all(b['pts_ms']>a['pts_ms'] and b['capture_us']>a['capture_us'] for a,b in zip(frames,frames[1:]))
    inferred={r['frame_mono_us'] for r in get('/api/detection/results')['results'] if r['mode']==1}
    matched=sum(f['capture_us'] in inferred for f in frames)
    assert matched>=10,(matched,frames)
    report['frame_clock']={'video_frames':len(frames),'exact_inference_matches':matched,'first_pts_ms':frames[0]['pts_ms']}
    for mode,name in [(3,'all'),(2,'objects')]:
        set_mode(mode);s=ready(mode);r=s['inference']['result']
        assert r['model']=='YOLOv5s-COCO80'
        if mode==2:assert all(b['class_id']!=0 for b in r['boxes'])
        report[name]=s['inference']
    set_mode(0);ready(0)
    # Performance counters are published by the 2s sampler, not immediately.
    time.sleep(2.2);a=get('/api/status');time.sleep(2.5);b=get('/api/status')
    assert a['performance']['counters'].get('cv_published',0)==b['performance']['counters'].get('cv_published',0)
    assert b['performance']['counters']['encoded_frames']>a['performance']['counters']['encoded_frames']
    assert not get('/api/detection/results')['results']
    report['off']={'inference':'disabled','publish_stopped':True,'video_continues':True}
    Path(sys.argv[2]).write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'frame_clock':report['frame_clock'],'off':report['off'],'modes':'person/all/objects/off passed'}))
finally:
    set_mode(original)

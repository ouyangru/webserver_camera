"""Run on PC with ADB forward active; verifies actual board video + NPU status.

python tests/cv_live_smoke.py http://127.0.0.1:18080 60 output.json
Does not measure model accuracy or glass-to-glass latency.
"""
import json
from pathlib import Path
import statistics
import sys
import time
from urllib.request import urlopen

base = sys.argv[1].rstrip('/')
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 60
output = Path(sys.argv[3]) if len(sys.argv) > 3 else Path('cv-live-report.json')
samples = []
start = time.monotonic()
while time.monotonic() - start < duration:
    with urlopen(base + '/api/status', timeout=5) as response:
        raw = response.read()
        assert len(raw) < 8192, 'status exceeds fixed HTTP response buffer'
        status = json.loads(raw)
    inference = status['inference']
    assert inference['available'], inference
    result = inference['result']
    assert result['state'] == 'running' and result['backend'] == 'NPU'
    assert status['streams']['flv']['ready'], status['streams']['flv']
    assert status['performance']['encoded_fps'] > 10, status['performance']
    assert 0 < result['run_ms'] < 1000
    if samples:
        assert result['frames'] > samples[-1]['inference']['result']['frames'], 'inference stalled'
    samples.append(status)
    time.sleep(2)
with urlopen(base + '/live.flv', timeout=5) as response:
    assert response.read(3) == b'FLV'
with urlopen(base + '/cv.html', timeout=5) as response:
    assert response.status == 200
rss = [s['inference']['result']['rss_kb'] for s in samples]
assert max(rss) - min(rss) < 8192, 'NPU worker RSS grew more than 8 MiB'
def median(path):
    values=[]
    for sample in samples:
        value=sample
        for key in path:
            value=value[key]
        values.append(value)
    return round(statistics.median(values), 3)
report = {
    'duration_s': round(time.monotonic()-start, 1), 'samples':len(samples),
    'video_fps_median':median(['performance','encoded_fps']),
    'video_cpu_pct_median':median(['performance','cpu_pct']),
    'video_rss_kb_median':median(['performance','rss_kb']),
    'infer_fps_median':median(['inference','result','fps']),
    'infer_cpu_pct_median':median(['inference','result','cpu_pct']),
    'infer_rss_kb_min':min(rss), 'infer_rss_kb_max':max(rss),
    'infer_run_ms_median':median(['inference','result','run_ms']),
    'frame_age_ms_median':median(['inference','result','frame_age_ms']),
    'flv_header':'valid', 'cv_page':'HTTP 200', 'snapshots':samples,
}
output.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k!='snapshots'},ensure_ascii=False))

"""Native HTTP/FLV smoke test. Hardware performance must be checked on board."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.request import urlopen

binary, fixture, resources = map(lambda s: str(Path(s).resolve()), sys.argv[1:4])
with tempfile.TemporaryDirectory(prefix="webserver-smoke-") as temp:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, WEBSERVER_CAMERA_RESOURCES=resources,
               WEBSERVER_LOG_FILE=temp + "/events.log", WEBSERVER_LOG_LEVEL="WARN")
    process = subprocess.Popen([binary, str(port), fixture], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    base = "http://127.0.0.1:" + str(port)
    def status():
        with urlopen(base + "/api/status", timeout=3) as response:
            return json.load(response)
    try:
        for _ in range(50):
            try:
                data = status()
                if data.get("performance", {}).get("rss_kb") and data["streams"]["flv"]["ready"]:
                    break
            except (OSError, ValueError):
                pass
            assert process.poll() is None, "server exited unexpectedly"
            time.sleep(0.1)
        else:
            raise AssertionError("metrics or FLV source did not become ready")
        before = data["performance"]["rss_kb"]
        with urlopen(base + "/live.flv", timeout=3) as response:
            assert response.status == 200 and response.read(3) == b"FLV"
        with urlopen(base + "/flv.min.js", timeout=3) as response:
            assert response.status == 200 and len(response.read()) > 10000
        for _ in range(100):
            assert "performance" in status()
        time.sleep(2.1)
        after = status()["performance"]["rss_kb"]
        assert after - before < 16384, (before, after)
        assert after < 65536, "native idle server unexpectedly retains a large connection table"
        print(json.dumps({"native_rss_before_kb": before, "native_rss_after_kb": after,
                          "http_requests": 100, "flv_header": "valid", "local_player": "ok"}))
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

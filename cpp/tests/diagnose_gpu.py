"""Serial end-to-end OCR probe; never benchmark CPU and GPU concurrently."""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

runtime, worker, output = map(Path, sys.argv[1:4])
output.mkdir(exist_ok=True, parents=True)
env = dict(os.environ, JJ_OCR_DIAGNOSTICS=str(output / "stages.jsonl"),
           JJ_OCR_MODELS=str(runtime / "models"), JJ_OV_CACHE=str(runtime / "ov-cache"),
           PYTHONDONTWRITEBYTECODE="1", PYTHONIOENCODING="utf-8")
images = [
    Path(r"C:\Users\Administrator\AppData\Local\Temp\ocr_test.png"),
    Path(r"C:\Users\Administrator\AppData\Local\Temp\codex-clipboard-c7f54f9a-fc1a-4e79-a22c-fcb467d71995.png"),
]
errors = (output / "stderr.txt").open("wb")
p = subprocess.Popen([str(runtime / "python.exe"), str(worker)],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, env=env)
(output / "pid.txt").write_text(str(p.pid), encoding="ascii")
records = []
monitor = None
if os.environ.get("JJ_CAPTURE_GPU") == "1":
    csv_path = str(output / "gpu-samples.jsonl").replace("'", "''")
    command = f"""
Get-Counter '\\GPU Engine(*)\\Utilization Percentage' -SampleInterval 1 -MaxSamples 120 -ErrorAction SilentlyContinue |
 ForEach-Object {{
  $stamp=$_.Timestamp.ToString('o');
  $_.CounterSamples | Where-Object {{$_.Status -eq 0 -and $_.InstanceName -like 'pid_{p.pid}_*'}} |
   ForEach-Object {{
    [pscustomobject]@{{time=$stamp;engine=$_.InstanceName;percent=$_.CookedValue}} |
     ConvertTo-Json -Compress | Add-Content -LiteralPath '{csv_path}'
   }}
 }}
"""
    monitor = subprocess.Popen(
        [str(Path(os.environ["SYSTEMROOT"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"),
         "-NoLogo", "-NoProfile", "-WindowStyle", "Hidden", "-Command", command],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NO_WINDOW)
try:
    assert p.stdout.readline().strip() == b"READY 1", "worker not ready"
    print(f"worker_pid={p.pid}", flush=True)
    for device in ("CPU", "GPU"):
        for image in images:
            for run in range(2):
                started = time.perf_counter()
                p.stdin.write(f"{device}\n{image}\n".encode("utf-8"))
                p.stdin.flush()
                header = p.stdout.readline()
                if not header:
                    raise RuntimeError(f"worker died: {p.poll()}")
                status, length = header.split()
                data = p.stdout.read(int(length))
                assert p.stdout.read(1) == b"\n"
                row = dict(device=device, image=image.name, run=run + 1, pid=p.pid,
                           status=status.decode(), elapsed_ms=round(
                               (time.perf_counter()-started)*1000, 1),
                           characters=len(data.decode("utf-8")))
                records.append(row)
                print(json.dumps(row), flush=True)
                assert status == b"OK", data.decode("utf-8", "replace")
finally:
    (output / "summary.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    if p.poll() is None:
        p.stdin.write(b"quit\n")
        p.stdin.flush()
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    errors.close()
    if monitor and monitor.poll() is None:
        monitor.terminate()
        monitor.wait(timeout=10)

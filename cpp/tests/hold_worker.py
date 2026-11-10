import os, subprocess, sys, time
runtime=sys.argv[1]; script=sys.argv[2]; out=sys.argv[3]
env=dict(os.environ, JJ_OCR_MODELS=runtime+r"\models", JJ_OV_CACHE=runtime+r"\ov-cache", JJ_OCR_DIAGNOSTICS=out+r"\diag.jsonl", PYTHONIOENCODING="utf-8")
p=subprocess.Popen([runtime+r"\python.exe",script],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,env=env)
open(out+r"\pid.txt","w").write(str(p.pid)); print("pid",p.pid,flush=True); print(p.stdout.readline().decode().strip(),flush=True)
for dev,img in [("CPU",r"C:\Users\Administrator\AppData\Local\Temp\ocr_test.png"),("GPU",r"C:\Users\Administrator\AppData\Local\Temp\ocr_test.png")]:
    p.stdin.write((dev+"\n"+img+"\n").encode()); p.stdin.flush(); h=p.stdout.readline().split(); print(dev,h,flush=True); p.stdout.read(int(h[1])); p.stdout.read(1)
    time.sleep(1)
print("HOLD",flush=True); time.sleep(30)
p.stdin.write(b"quit\n"); p.stdin.flush(); p.wait()

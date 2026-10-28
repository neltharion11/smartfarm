import urllib.request, os, time, zipfile, shutil

DEST = r"F:\android-tools"
os.makedirs(DEST, exist_ok=True)
url = "https://services.gradle.org/distributions/gradle-9.7.1-bin.zip"
zp = os.path.join(DEST, "gradle-9.7.1-bin.zip")

print("=== 下载 Gradle 9.7.1 ===", flush=True)
if not (os.path.exists(zp) and os.path.getsize(zp) > 10_000_000):
    req = urllib.request.Request(url, headers={"User-Agent":"Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=60) as r, open(zp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        got = 0; t0 = time.time()
        while True:
            c = r.read(1 << 20)
            if not c: break
            f.write(c); got += len(c)
            if got % (10 << 20) < (1 << 20):
                print(f"    {got/1048576:6.1f} MB" + (f" / {total/1048576:.1f} MB" if total else ""), flush=True)
        print(f"    完成 {got/1048576:.1f} MB, {time.time()-t0:.0f}s", flush=True)
else:
    print("    已存在，跳过", flush=True)

print("=== 解压 ===", flush=True)
outdir = os.path.join(DEST, "gradle-9.7.1")
if not os.path.isdir(outdir):
    with zipfile.ZipFile(zp) as z:
        z.extractall(DEST)
print("    ", outdir, "存在:", os.path.isdir(outdir))
print("    版本自检:", flush=True)

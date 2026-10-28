import urllib.request, os, time

OUT = r"F:\android-tools\downloads"
os.makedirs(OUT, exist_ok=True)

ITEMS = [
    ("JDK 21 LTS (Temurin)", "https://api.adoptium.net/v3/binary/latest/21/ga/windows/x64/jdk/hotspot/normal/eclipse", "jdk21.zip"),
    ("Android cmdline-tools", "https://dl.google.com/android/repository/commandlinetools-win-16111833_latest.zip", "cmdline-tools.zip"),
]

for name, url, fname in ITEMS:
    dst = os.path.join(OUT, fname)
    if os.path.exists(dst) and os.path.getsize(dst) > 1_000_000:
        print(f"=== {name} === 已存在，跳过 ({os.path.getsize(dst):,} bytes)")
        continue
    print(f"=== {name} ===")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=60) as r, open(dst, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got = 0
            t0 = time.time()
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                print(f"    {got/1048576:7.1f} MB" + (f" / {total/1048576:.1f} MB" if total else ""), flush=True)
            dt = time.time() - t0
            print(f"    完成 {got/1048576:.1f} MB，{dt:.0f}s，{got/1048576/max(dt,1):.2f} MB/s")
    except Exception as e:
        print(f"    失败: {type(e).__name__}: {e}")

print("=== 下载目录 ===")
for f in sorted(os.listdir(OUT)):
    print(f"    {f}  {os.path.getsize(os.path.join(OUT,f)):,} bytes")

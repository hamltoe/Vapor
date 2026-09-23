import hashlib, os, zipfile, subprocess, json, sqlite3

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest(), os.path.getsize(p)

paths = {
    "iso": "/mnt/g/Vapor/Library/Half-Life/Half-Life Game Of The Year Edition.iso",
    "zip": "/home/ton_o/vapor/content/half-life/1789794284/package.zip",
    "install": "/mnt/c/Users/ton_o/AppData/Local/Vapor/Games/half-life",
}

z = zipfile.ZipFile(paths["zip"])
names = z.namelist()
hl = [n for n in names if n.lower().replace("\\","/").endswith("hl.exe") or n.lower().endswith("hl.exe")]
print("zip hl.exe entries", hl)
for n in hl:
    info = z.getinfo(n)
    data = z.read(n)
    print(" zip", n, "size", info.file_size, "sha", hashlib.sha256(data).hexdigest())

# extract iso hl.exe via python iso parser from earlier logic if needed
inst = paths["install"]
if os.path.isdir(inst):
    print("install dir listing (root exes/dlls):")
    for n in sorted(os.listdir(inst)):
        p = os.path.join(inst, n)
        if os.path.isfile(p) and n.lower().endswith((".exe", ".dll")):
            d, s = sha(p)
            print(f"  {n:20} {s:8} {d}")
    for needle in ("HL.EXE", "hl.exe", "WONAuth.dll", "hw.dll", "HW.DLL", "sw.dll", "SW.DLL"):
        p = os.path.join(inst, needle)
        if os.path.exists(p):
            d, s = sha(p)
            print("found", needle, s, d)
else:
    print("no install dir", inst)

c = sqlite3.connect("/home/ton_o/vapor/vapor.db")
js = c.execute("select manifest_json from versions where game_id='half-life'").fetchone()
if js:
    j = json.loads(js[0])
    print("manifest exec", [t.get("exec") for t in j.get("targets") or []])
    print("package size", j.get("package", {}).get("size"))

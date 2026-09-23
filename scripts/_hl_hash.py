import hashlib
import os
import struct
import zipfile

SECTOR = 2048

def u32le(b, o):
    return struct.unpack_from("<I", b, o)[0]

def decode_name(name, nlen, joliet):
    if nlen == 1 and name[0] in (0, 1):
        return None
    if joliet:
        if nlen < 2:
            return "?"
        if nlen & 1:
            nlen -= 1
        chars = []
        for i in range(0, nlen, 2):
            cp = (name[i] << 8) | name[i + 1]
            if cp in (0, ord(";")):
                break
            chars.append(chr(cp) if cp < 0x10000 else "?")
        return "".join(chars)
    s = name[:nlen].split(b";", 1)[0]
    if s.endswith(b"."):
        s = s[:-1]
    return s.decode("ascii", "replace")

def walk(f, lba, size, rel, joliet, depth, out):
    if depth > 16 or size == 0:
        return
    f.seek(lba * SECTOR)
    data = f.read(size)
    off = 0
    while off < len(data):
        rec = data[off]
        if rec == 0:
            off = ((off // SECTOR) + 1) * SECTOR
            continue
        if rec < 34 or off + rec > len(data):
            break
        flags = data[off + 25]
        nlen = data[off + 32]
        name = decode_name(data[off + 33 : off + 33 + nlen], nlen, joliet)
        flba = u32le(data, off + 2)
        flen = u32le(data, off + 10)
        off += rec
        if not name:
            continue
        path = f"{rel}/{name}" if rel else name
        out.append((path, flen, flags, flba))
        if flags & 0x02:
            walk(f, flba, flen, path, joliet, depth + 1, out)

def iso_file(iso, want):
    with open(iso, "rb") as f:
        pvd = svd = None
        for i in range(16, 32):
            f.seek(i * SECTOR)
            sec = f.read(SECTOR)
            if sec[0] == 1:
                pvd = sec
            elif sec[0] == 2 and sec[88:90] == b"%/":
                svd = sec
            elif sec[0] == 255:
                break
        root = svd or pvd
        joliet = svd is not None
        files = []
        walk(f, u32le(root, 158), u32le(root, 166), "", joliet, 0, files)
        want_l = want.lower()
        for path, size, flags, lba in files:
            if path.lower() == want_l and not (flags & 2):
                f.seek(lba * SECTOR)
                return f.read(size), path, size
    return None, None, None

iso = "/mnt/g/Vapor/Library/Half-Life/Half-Life Game Of The Year Edition.iso"
zpath = "/home/ton_o/vapor/content/half-life/1789794284/package.zip"
data, name, size = iso_file(iso, "HL.EXE")
print("iso", name, size, hashlib.sha256(data).hexdigest() if data else None)
z = zipfile.ZipFile(zpath)
zd = z.read("HL.EXE")
print("zip HL.EXE", len(zd), hashlib.sha256(zd).hexdigest(), "match", data == zd)
# interesting files
for n in sorted(z.namelist()):
    ln = n.lower()
    if any(x in ln for x in ("kver", "won", "hl.exe", "hw.dll", "sw.dll", "hl.dat", "woncomm")):
        info = z.getinfo(n)
        print(f"{info.file_size:10} {n}")
# extract kver and woncomm
os.makedirs("/tmp/hlpkg", exist_ok=True)
for n in z.namelist():
    ln = n.lower().replace("\\", "/")
    if ln.endswith(("kver.kp", "woncomm.lst", "hl.dat", "version.txt")):
        raw = z.read(n)
        outp = "/tmp/hlpkg/" + os.path.basename(n)
        open(outp, "wb").write(raw)
        print("wrote", outp, len(raw), raw[:80])

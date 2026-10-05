"""Out-of-process memory search in SRTTR.exe (reverse-engineering aid; can't crash the game).

  python tools/memscan.py vec X Y Z [--tol 0.001]     float triples ~= (X, Y, Z) in writable memory
  python tools/memscan.py read ADDR [N]               N floats at ADDR (hex)
  python tools/memscan.py addx FILE DX [--only i,j-k]  add DX to the x float at each address in FILE (vec output)
  python tools/memscan.py near X Y Z RMIN RMAX OUT.npy   float triples at distance RMIN..RMAX from (X,Y,Z), 16-byte aligned
  python tools/memscan.py snap IN.npy OUT.npy           re-read the triples at IN's addresses (column 0) into OUT
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess

import numpy as np

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ, PROCESS_VM_WRITE, PROCESS_VM_OPERATION, PROCESS_QUERY_INFORMATION = 0x10, 0x20, 0x8, 0x400
MEM_COMMIT, MEM_IMAGE = 0x1000, 0x1000000
WRITABLE = 0x04 | 0x08 | 0x40 | 0x80  # RW, WRITECOPY, EXECUTE_RW, EXECUTE_WRITECOPY
PAGE_GUARD = 0x100


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


def sr3_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq SRTTR.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith('"SRTTR.exe"'):
            return int(line.split('","')[1])
    raise SystemExit("SRTTR.exe isn't running")


def open_game():
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, False, sr3_pid())
    if not h:
        raise SystemExit(f"OpenProcess failed ({ctypes.get_last_error()})")
    return h


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    return buf.raw[:got.value]


def regions(h):
    addr, mbi = 0, MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        if mbi.State == MEM_COMMIT and mbi.Protect & WRITABLE and not mbi.Protect & PAGE_GUARD:
            yield mbi.BaseAddress, mbi.RegionSize, mbi.Type == MEM_IMAGE
        addr = mbi.BaseAddress + mbi.RegionSize
        if addr >= 1 << 47:
            break


def find_vec(h, want, tol):
    hits = []
    for base, size, image in regions(h):
        for off in range(0, size, 64 << 20):  # 64 MB chunks
            data = read(h, base + off, min(64 << 20, size - off))
            n = len(data) // 4
            if n < 3:
                continue
            f = np.frombuffer(data[:n * 4], dtype=np.float32)
            m = (np.abs(f[:-2] - want[0]) < tol) & (np.abs(f[1:-1] - want[1]) < tol) & (np.abs(f[2:] - want[2]) < tol)
            hits += [(base + off + 4 * int(i), image) for i in np.nonzero(m)[0]]
    return hits


def find_near(h, c, rmin, rmax):
    """16-byte aligned float triples (x, y, z) with rmin <= |p - c| <= rmax: rows of [address, x, y, z]."""
    rows = []
    for base, size, image in regions(h):
        for off in range(0, size, 64 << 20):
            data = read(h, base + off, min(64 << 20, size - off))
            n = len(data) // 16
            if n == 0:
                continue
            v = np.frombuffer(data[:n * 16], dtype=np.float32).reshape(n, 4)[:, :3]
            with np.errstate(invalid="ignore", over="ignore"):
                d = np.sqrt(((v - np.float32(c)) ** 2).sum(axis=1))
            idx = np.nonzero((d >= rmin) & (d <= rmax))[0]
            for i in idx:
                rows.append((base + off + 16 * int(i), *v[i]))
    return np.array(rows, dtype=np.float64).reshape(-1, 4)


def snap(h, rows):
    out = rows.copy()
    for k, addr in enumerate(rows[:, 0].astype(np.int64)):
        b = read(h, int(addr), 12)
        out[k, 1:] = np.frombuffer(b, dtype=np.float32) if len(b) == 12 else np.nan
    return out


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("vec")
    v.add_argument("xyz", type=float, nargs=3)
    v.add_argument("--tol", type=float, default=0.001)
    r = sub.add_parser("read")
    r.add_argument("addr")
    r.add_argument("n", type=int, nargs="?", default=4)
    nr = sub.add_parser("near")
    nr.add_argument("xyz", type=float, nargs=3)
    nr.add_argument("rmin", type=float)
    nr.add_argument("rmax", type=float)
    nr.add_argument("out")
    sn = sub.add_parser("snap")
    sn.add_argument("inp")
    sn.add_argument("out")
    w = sub.add_parser("addx")
    w.add_argument("file")
    w.add_argument("dx", type=float)
    w.add_argument("--only", default="")
    a = ap.parse_args()
    h = open_game()
    if a.cmd == "vec":
        for addr, image in find_vec(h, a.xyz, a.tol):
            print(f"{addr:x} {'image' if image else 'heap'}")
    elif a.cmd == "near":
        rows = find_near(h, a.xyz, a.rmin, a.rmax)
        np.save(a.out, rows)
        print(f"{len(rows)} triples")
    elif a.cmd == "snap":
        np.save(a.out, snap(h, np.load(a.inp)))
    elif a.cmd == "addx":
        addrs = [int(l.split()[0], 16) for l in open(a.file) if l.strip()]
        if a.only:
            keep = set()
            for part in a.only.split(","):
                lo, _, hi = part.partition("-")
                keep.update(range(int(lo), int(hi or lo) + 1))
            addrs = [x for i, x in enumerate(addrs) if i in keep]
        for addr in addrs:
            x = np.frombuffer(read(h, addr, 4), dtype=np.float32)[0] + a.dx
            k32.WriteProcessMemory(h, ctypes.c_void_p(addr), np.float32(x).tobytes(), 4, None)
        print(f"wrote {len(addrs)}")
    else:
        data = read(h, int(a.addr, 16), 4 * a.n)
        print(" ".join(f"{x:.4f}" for x in np.frombuffer(data, dtype=np.float32)))


if __name__ == "__main__":
    main()

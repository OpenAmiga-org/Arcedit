#!/home/claude/build/amiga-fleet-venv/bin/python
"""run_os4.py: arcedit on AmigaOS 4 under awkward conditions, driven from
the host through MCPd (amiga_fleet_mcp), results checked on the host.

Needs: bin/os4/arcedit ("make os4"), tests/os4/memeater and holdlock
(cross-compiled from the .c files next to this script), a running OS4
guest reachable as target "os4-amigaone".

Scenarios:
 1. Linux vs OS4: the same edits on every sample archive with both builds,
    the results must be byte for byte the same
 2. Ctrl-C while a big archive is rewritten
 3. an archive another program holds locked
 4. a write-protected volume
 5. a full volume (a 1 MB RAM drive)
 6. a 30 character archive name on FFS
 7. low memory (memeater leaves 24 MB of private memory)
 8. edits on Work: and System: (their own file systems)
"""
import asyncio
import hashlib
import os
import re
import shutil
import subprocess
import sys

from amiga_fleet_mcp.config import load_config
from amiga_fleet_mcp.fleet import Fleet
from amiga_fleet_mcp.tools import fs as fs_tool

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST_AE = os.path.join(ROOT, "bin", "arcedit")
OS4_AE = os.path.join(ROOT, "bin", "os4", "arcedit")
SAMPLES = os.path.join(ROOT, "tests", "samples")
WORK = os.path.join(ROOT, "tests", "work", "os4")
TARGET = "os4-amigaone"
G = "RAM:os4t"            # guest work drawer

results = []


def ok(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"  {detail}"), flush=True)


class Guest:
    def __init__(self):
        self.fleet = Fleet(load_config())
        self.m = self.fleet.mcpd(TARGET)

    async def cmd(self, command, timeout=120):
        r = await self.m.request("exec.cmd", {"command": command, "timeout_ms": timeout * 1000},
                                 timeout_s=timeout + 30)
        return r.get("exit_code", -1), r.get("output", "")

    async def up(self, local, remote):
        await fs_tool.fs_upload(self.fleet, TARGET, local_path=local, remote_path=remote, verify=True)

    async def down(self, remote, local):
        await fs_tool.fs_download(self.fleet, TARGET, remote_path=remote, local_path=local)

    async def sha(self, remote):
        os.makedirs(WORK, exist_ok=True)
        tmp = os.path.join(WORK, "sha.tmp")
        if os.path.exists(tmp):
            os.remove(tmp)
        try:
            await self.down(remote, tmp)
        except Exception:
            return None
        return hashlib.sha1(open(tmp, "rb").read()).hexdigest()

    async def leftovers(self, drawer):
        rc, out = await self.cmd(f"List {drawer} PAT=ae#?.tmp QUICK NOHEAD")
        return [l for l in out.splitlines() if l.strip().lower().startswith("ae")]

    async def ae(self, args, timeout=300):
        return await self.cmd(f"{G}/arcedit <NIL: {args}", timeout)


def host(args):
    p = subprocess.run([HOST_AE] + args, capture_output=True)
    return p.returncode


# edits per sample: (name, arguments after the archive)
EDITS = [
    ("comment", ["a.txt", "COMMENT", "os4 note"]),
    ("protect", ["sub", "PROTECT", "+s-w"]),
    ("rename", ["RENAME", "sub", "AS", "Drawer"]),
    ("delete", ["a.txt", "DELETE"]),
]


async def differential(g):
    print("-- 1. Linux vs OS4")
    for f in sorted(os.listdir(SAMPLES)):
        src = os.path.join(SAMPLES, f)
        for name, args in EDITS:
            hl = os.path.join(WORK, f"h-{name}-{f}")
            shutil.copy(src, hl)
            hrc = host([hl] + args + ["QUIET"])
            await g.up(src, f"{G}/{f}")
            qargs = " ".join(f'"{a}"' if " " in a else a for a in args)
            grc, out = await g.ae(f"{G}/{f} {qargs} QUIET")
            gl = os.path.join(WORK, f"g-{name}-{f}")
            await g.down(f"{G}/{f}", gl)
            same = open(hl, "rb").read() == open(gl, "rb").read()
            ok(f"{f} {name}: same return code ({hrc}/{grc}) and bytes", hrc == grc and same, out.strip()[-200:])


async def ctrl_c(g):
    print("-- 2. Ctrl-C")
    big = os.path.join(WORK, "big.zip")
    if not os.path.exists(big):
        d = os.path.join(WORK, "bigsrc")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "zeros"), "wb") as fh:
            fh.write(b"\0" * 150_000_000)
        with open(os.path.join(d, "small"), "wb") as fh:
            fh.write(b"x")
        subprocess.run(["zip", "-q", "-0", big, "zeros", "small"], cwd=d)
    await g.up(big, f"{G}/big.zip")
    before = await g.sha(f"{G}/big.zip")
    await g.cmd(f"Run >{G}/brk.txt {G}/arcedit <NIL: {G}/big.zip small DELETE")
    await asyncio.sleep(0.4)
    rc, out = await g.cmd("Status COMMAND=" + G + "/arcedit")
    num = re.search(r"(\d+)", out)
    if num:
        await g.cmd(f"Break {num.group(1)} C")
    for _ in range(60):
        rc, out = await g.cmd("Status COMMAND=" + G + "/arcedit")
        if not re.search(r"\d", out):
            break
        await asyncio.sleep(1)
    rc, msg = await g.cmd(f"Type {G}/brk.txt")
    after = await g.sha(f"{G}/big.zip")
    ok("ctrl-c: archive unchanged", before == after)
    ok("ctrl-c: no temporary file", not await g.leftovers(G))
    ok("ctrl-c: 'interrupted' reported", "interrupted" in msg or not num, msg.strip()[-200:])
    await g.cmd(f"Delete {G}/big.zip QUIET")


async def locked(g):
    print("-- 3. locked archive")
    await g.up(os.path.join(SAMPLES, "h1.lha"), f"{G}/lock.lha")
    await g.cmd(f"Run >NIL: {G}/holdlock {G}/lock.lha")
    await asyncio.sleep(1)
    rc, out = await g.ae(f"{G}/lock.lha INFO")
    ok("locked: INFO fails with a reason", rc == 10 and "Error:" in out and "doesn't exist" not in out, out.strip())
    rc, out = await g.ae(f"{G}/lock.lha a.txt DELETE")
    ok("locked: DELETE fails cleanly", rc == 10 and "Error:" in out and not await g.leftovers(G), out.strip())
    rc, out = await g.cmd("Status COMMAND=" + G + "/holdlock")
    num = re.search(r"(\d+)", out)
    if num:
        await g.cmd(f"Break {num.group(1)} C")


async def tiny_volume(g):
    """A 1 MB RAM drive TINY: from RAD:'s mountlist"""
    rc, out = await g.cmd("Assign TINY: EXISTS")
    if rc == 0:
        return True
    for ml in ("DEVS:DOSDrivers/RAD", "SYS:Storage/DOSDrivers/RAD"):
        rc, text = await g.cmd(f"Type {ml}")
        if rc == 0 and "ramdrive" in text.lower():
            break
    else:
        return False
    # RAD's file leaves Unit/LowCyl/HighCyl to icon tooltypes; a classic
    # mountlist entry with them filled in (90 cylinders x 11 KB, about 1 MB)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(?im)^\s*(Unit|LowCyl|HighCyl|Activate|BootPri)\s*=.*$", "", text)
    text = "\n".join(l for l in text.splitlines() if l.strip())
    path = os.path.join(WORK, "TINY.mountlist")
    with open(path, "w") as fh:
        fh.write("TINY:\n" + text + "\nUnit = 3\nLowCyl = 0\nHighCyl = 89\nActivate = 1\n#\n")
    await g.up(path, f"{G}/TINY")
    rc, out = await g.cmd(f"Mount TINY: FROM {G}/TINY")
    rc2, out2 = await g.cmd("Info TINY:")
    print(out2.strip()[-300:])
    return rc == 0


async def write_protected(g, have_tiny):
    print("-- 4. write-protected volume")
    if not have_tiny:
        ok("write-protected: TINY: volume available", False, "no RAD mountlist")
        return
    await g.cmd("Delete TINY:#? ALL QUIET")
    await g.up(os.path.join(SAMPLES, "h1.lha"), "TINY:wp.lha")
    await g.cmd("Lock TINY: ON")
    rc, out = await g.ae("TINY:wp.lha INFO")
    ok("write-protected: INFO lists nothing", rc == 0 and out.strip().endswith("LHA:"), out.strip())
    rc, out = await g.ae("TINY:wp.lha a.txt DELETE")
    ok("write-protected: DELETE refused", rc == 10 and "write protected" in out, out.strip())
    await g.cmd("Lock TINY: OFF")


async def full_volume(g, have_tiny):
    print("-- 5. full volume")
    if not have_tiny:
        ok("full: TINY: volume available", False)
        return
    await g.cmd("Delete TINY:#? ALL QUIET")
    big = os.path.join(WORK, "rand.bin")
    with open(big, "wb") as fh:
        fh.write(os.urandom(1_500_000))   # more than TINY: (about 1 MB) holds
    await g.up(big, f"{G}/rand.bin")
    await g.up(os.path.join(SAMPLES, "real_lzB.lzx"), "TINY:full.lzx")
    before = await g.sha("TINY:full.lzx")
    rc, out = await g.ae(f"TINY:full.lzx ADD {G}/rand.bin")
    ok("full: ADD refused for space", rc == 10 and "free space" in out, out.strip())
    ok("full: archive unchanged, no temp", before == await g.sha("TINY:full.lzx") and not await g.leftovers("TINY:"))
    # fill the rest so even a DELETE (a copy of the archive) can't fit
    rc, out = await g.cmd("Info TINY:")
    await g.cmd(f"Copy {G}/rand.bin TINY:filler")
    rc, out = await g.ae("TINY:full.lzx S/Startup-Sequence-preUpd2 DELETE")
    ok("full: DELETE refused or done cleanly", (rc == 10 and "space" in out) or rc in (0, 10),
       out.strip())
    ok("full: no temp after DELETE", not await g.leftovers("TINY:"))


async def ffs_name(g, have_tiny):
    print("-- 6. 30 character name on FFS")
    if not have_tiny:
        ok("ffs: TINY: volume available", False)
        return
    await g.cmd("Delete TINY:#? ALL QUIET")
    name = "TINY:" + "n" * 26 + ".lha"   # 30 characters
    await g.up(os.path.join(SAMPLES, "h1.lha"), name)
    rc, out = await g.ae(f"{name} a.txt COMMENT ffs")
    ok("ffs: edit with a 30 character name", rc == 0, out.strip())
    rc, out = await g.cmd(f"LhA t {name}")
    ok("ffs: LhA t", rc == 0, out.strip()[-200:])


async def low_memory(g):
    """memeater takes all private memory and gives 24 MB back (less starves
    MCPd, the test's own link to the guest); arcedit then adds 40 MB. Each operation must either work
    (LhA t passes) or fail with an Error and leave the archive unchanged."""
    print("-- 7. low memory")
    big = os.path.join(WORK, "rand8.bin")
    with open(big, "wb") as fh:
        fh.write(os.urandom(8_000_000))
    await g.up(big, f"{G}/rand8.bin")
    r8 = f"{G}/rand8.bin"
    await g.cmd(f"Join {r8} {r8} {r8} {r8} {r8} AS {G}/rand40.bin")
    await g.up(os.path.join(SAMPLES, "h1.lha"), f"{G}/mem.lha")
    await g.up(os.path.join(SAMPLES, "g_tar.tgz"), f"{G}/mem.tgz")
    before = await g.sha(f"{G}/mem.lha")
    await g.cmd(f"Delete {G}/eat.txt QUIET")
    await g.cmd(f"Run >NIL: {G}/memeater 24000 {G}/eat.txt")
    eat = ""
    for _ in range(30):
        await asyncio.sleep(2)
        rc, eat = await g.cmd(f"Type {G}/eat.txt")
        if "holding" in eat:
            break
    print(eat.strip())
    ok("low memory: memeater holds the memory", "holding" in eat, eat.strip())

    rc, out = await g.ae(f"{G}/mem.lha ADD {G}/rand40.bin", timeout=600)
    print(f"  ADD: rc {rc}: {out.strip()[-200:]}")
    if rc == 0:
        rct, _ = await g.cmd(f"LhA t {G}/mem.lha")
        ok("low memory: ADD of 40 MB worked, LhA t passes", rct == 0, out.strip())
    else:
        ok("low memory: ADD of 40 MB failed cleanly", rc in (10, 20) and "Error:" in out, out.strip())
        ok("low memory: archive unchanged", before == await g.sha(f"{G}/mem.lha"))
    ok("low memory: no temp", not await g.leftovers(G + "/"))
    rc, out = await g.ae(f"{G}/mem.lha a.txt DELETE")
    ok("low memory: DELETE works", rc == 0, out.strip())
    rc, out = await g.ae(f"{G}/mem.tgz a.txt COMMENT x")
    ok("low memory: small TGZ edit", rc == 0, out.strip())
    rc, out = await g.cmd("Status COMMAND=" + G + "/memeater")
    for num in re.findall(r"(\d+)", out):
        await g.cmd(f"Break {num} C")
    await asyncio.sleep(3)
    rc, eat = await g.cmd(f"Type {G}/eat.txt")
    ok("low memory: memeater gave the memory back", "given back" in eat, eat.strip())
    await g.cmd(f"Delete {G}/rand40.bin {G}/rand8.bin QUIET")


async def volumes(g):
    print("-- 8. Work: and System:")
    for vol in ("Work:", "SYS:"):   # SYS:T may not exist; the test file is deleted again
        dst = f"{vol}ae_vol_test.lha"
        await g.up(os.path.join(SAMPLES, "h2.lha"), dst)
        rc1, o1 = await g.ae(f"{dst} a.txt COMMENT vol")
        rc2, o2 = await g.ae(f"{dst} RENAME sub AS Drawer")
        rc3, o3 = await g.cmd(f"LhA t {dst}")
        ok(f"{vol} edits", rc1 == 0 and rc2 == 0 and rc3 == 0, (o1 + o2 + o3).strip()[-300:])
        await g.cmd(f"Delete {dst} QUIET")


async def main():
    os.makedirs(WORK, exist_ok=True)
    g = Guest()
    await g.cmd(f"MakeDir {G}")
    await g.up(OS4_AE, f"{G}/arcedit")
    for tool in ("memeater", "holdlock"):
        await g.up(os.path.join(ROOT, "tests", "os4", tool), f"{G}/{tool}")
    await differential(g)
    await ctrl_c(g)
    await locked(g)
    have_tiny = await tiny_volume(g)
    await write_protected(g, have_tiny)
    await full_volume(g, have_tiny)
    await ffs_name(g, have_tiny)
    await low_memory(g)
    await volumes(g)
    failed = [n for n, r in results if not r]
    print(f"os4: {len(results)} checks, {len(failed)} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    asyncio.run(main())

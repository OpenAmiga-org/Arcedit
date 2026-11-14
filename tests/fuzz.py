#!/usr/bin/env python3
"""fuzz.py [ITERATIONS] [SEED]: feed damaged archives to arcedit built with
AddressSanitizer/UBSan (bin/arcedit-asan, "make asan") and check that it
never misbehaves:

 - no sanitizer report, no crash, no hang (10 s)
 - return code 0, 5, 10 or 20
 - on 10/20 the archive is byte for byte unchanged
 - no temporary files left behind
 - after 0/5 arcedit can open its own result again

The damage: bit flips, bytes set to 0/0xFF/random (often in the first
512 bytes, where the headers are), truncation, inserted or repeated
chunks. Failing inputs are kept in tests/work/fuzz-fail/.
"""
import os
import random
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AE = os.path.join(ROOT, "bin", "arcedit-asan")
SAMPLES = os.path.join(ROOT, "tests", "samples")
WORK = os.path.join(ROOT, "tests", "work", "fuzz")
FAIL = os.path.join(ROOT, "tests", "work", "fuzz-fail")
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=99",
           UBSAN_OPTIONS="halt_on_error=1:exitcode=98:print_stacktrace=1")

OPS = [
    ["INFO"],
    ["a.txt", "COMMENT", "fuzz note"],
    ["sub", "PROTECT", "+s-w"],
    ["RENAME", "a.txt", "AS", "x/renamed.txt"],
    ["RENAME", "sub", "AS", "other"],
    ["sub", "DELETE"],
    ["a.txt", "DELETE"],
    ["ADD", os.path.join(ROOT, "Makefile"), "TO", "added"],
]


def mutate(data, rnd):
    data = bytearray(data)
    for _ in range(rnd.randint(1, 6)):
        if not data:
            data = bytearray(rnd.randbytes(rnd.randint(1, 64)))
            continue
        kind = rnd.randrange(7)
        # most damage near the start, where headers are
        pos = rnd.randrange(min(len(data), 512)) if rnd.random() < 0.7 else rnd.randrange(len(data))
        if kind == 0:
            data[pos] ^= 1 << rnd.randrange(8)
        elif kind == 1:
            data[pos] = rnd.choice((0, 0xFF, 0x7F, 0x80, rnd.randrange(256)))
        elif kind == 2:
            data = data[:rnd.randrange(len(data))]
        elif kind == 3:
            data[pos:pos] = rnd.randbytes(rnd.randint(1, 32))
        elif kind == 4:
            n = rnd.randint(1, 64)
            data[pos:pos] = data[pos:pos + n]
        elif kind == 5:
            n = rnd.randint(1, 16)
            del data[pos:pos + n]
        else:
            # a size field style value: 4 bytes
            v = rnd.choice((0, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, rnd.randrange(1 << 32)))
            data[pos:pos + 4] = v.to_bytes(4, rnd.choice(("little", "big")))
    return bytes(data)


def run(args, timeout=10):
    try:
        p = subprocess.run([AE] + args, capture_output=True, timeout=timeout, env=ENV, text=True,
                           errors="replace")
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT"


def one(i, seed, samples):
    rnd = random.Random(seed * 1000003 + i)
    src = rnd.choice(samples)
    ext = os.path.splitext(src)[1]
    if src.endswith(".tar.gz"):
        ext = ".tar.gz"
    d = os.path.join(WORK, str(i % 64))
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        os.remove(os.path.join(d, f))
    arc = os.path.join(d, "f" + ext)
    data = mutate(open(src, "rb").read(), rnd)
    with open(arc, "wb") as fh:
        fh.write(data)
    op = rnd.choice(OPS)
    rc, out = run([arc] + op + ["QUIET"])
    problem = None
    if rc is None:
        problem = "hang"
    elif "AddressSanitizer" in out or "runtime error" in out or "LeakSanitizer" in out or rc in (98, 99):
        problem = "sanitizer"
    elif rc not in (0, 5, 10, 20):
        problem = f"return code {rc}"
    elif rc >= 10 and open(arc, "rb").read() != data:
        problem = "archive changed after an error"
    elif any(f.endswith(".tmp") for f in os.listdir(d)):
        problem = "temporary file left"
    elif rc in (0, 5) and op[0] != "INFO":
        rc2, out2 = run([arc, "no-such-entry", "COMMENT", "x", "QUIET"])
        if rc2 not in (5,):
            problem = f"own result can't be opened again (rc {rc2}): {out2.strip()[-200:]}"
            out = out2
    if problem:
        os.makedirs(FAIL, exist_ok=True)
        keep = os.path.join(FAIL, f"{i}-{os.path.basename(src)}")
        with open(keep, "wb") as fh:
            fh.write(data)
        return f"{problem}: {os.path.basename(src)} op={' '.join(op)} kept={keep}\n{out[-1500:]}"
    return None


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    samples = [os.path.join(SAMPLES, f) for f in sorted(os.listdir(SAMPLES))]
    shutil.rmtree(WORK, ignore_errors=True)
    problems = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for r in ex.map(lambda i: one(i, seed, samples), range(n)):
            if r:
                problems.append(r)
                print(r, flush=True)
    print(f"fuzz: {n} runs, {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

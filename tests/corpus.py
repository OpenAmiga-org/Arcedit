#!/usr/bin/env python3
"""corpus.py DIR: run arcedit's operations on every archive in DIR (real
archives from Aminet, OS4Depot, ...) and check each result with the
format's reference tool (Lhasa for LHA, unzip, GNU tar, gzip, unar for
LZX):

 - the tool's test must pass on the result,
 - every file must extract byte for byte as from the untouched archive,
   except the one the operation changed (renamed, deleted, added).

Operations, each on a fresh copy and only when INFO lists it: COMMENT,
PROTECT, RENAME (first file), DELETE (every second file, from a list
file), ADD (a known file).

Archives arcedit refuses although the tool reads them are listed as
"refused", archives the tool itself can't test as "skipped (damaged)".
"""
import hashlib
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AE = os.path.join(ROOT, "bin", "arcedit")
WORK = os.path.join(ROOT, "tests", "work", "corpus")
ADDFILE = os.path.join(ROOT, "src", "lha.c")


def sh(cmd, timeout=600):
    p = subprocess.run(cmd, capture_output=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr).decode("latin-1")


def kind(arc):
    n = os.path.basename(arc).lower()
    if n.endswith((".tar.gz", ".tgz")):
        return "tgz"
    for e, k in ((".lha", "lha"), (".lzh", "lha"), (".zip", "zip"), (".tar", "tar"), (".lzx", "lzx"), (".gz", "gz")):
        if n.endswith(e):
            return k
    return None


def test_ok(arc):
    k = kind(arc)
    if k == "lha":
        return sh(["lha", "t", arc])[0] == 0
    if k == "zip":
        return sh(["unzip", "-tqq", arc])[0] == 0
    if k == "tar":
        return sh(["tar", "-tf", arc])[0] == 0
    if k == "tgz":
        return sh(["tar", "-tzf", arc])[0] == 0
    if k == "gz":
        return sh(["gzip", "-t", arc])[0] == 0
    rc, out = sh(["lsar", "-t", arc])
    return rc == 0 and " 0 failed" in out


def tree(arc, dest):
    """Extract arc into dest with the reference tool; {relative path: sha1}"""
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    k = kind(arc)
    if k == "lha":
        sh(["lha", "xqfw=" + dest, arc])
    elif k == "zip":
        sh(["unzip", "-qqo", arc, "-d", dest])
    elif k == "tar":
        sh(["tar", "-xf", arc, "-C", dest])
    elif k == "tgz":
        sh(["tar", "-xzf", arc, "-C", dest])
    elif k == "gz":
        with open(os.path.join(dest, "content"), "wb") as fh:
            subprocess.run(["gzip", "-dc", arc], stdout=fh)
    else:
        sh(["unar", "-q", "-D", "-o", dest, arc])
    files = {}
    for d, dirs, fs in os.walk(dest):
        for x in dirs:
            os.chmod(os.path.join(d, x), 0o755)
        for f in fs:
            p = os.path.join(d, f)
            if os.path.islink(p):
                continue
            os.chmod(p, 0o644)
            files[os.path.relpath(p, dest)] = hashlib.sha1(open(p, "rb").read()).hexdigest()
    return files


def raw(n):
    """The name as bytes, as it is in the archive (Latin-1 on the Amiga)"""
    return os.fsencode(n)


def check(name, arc, base, expect_missing=(), expect_moved=None, expect_added=None):
    if not test_ok(arc):
        return f"{name}: the reference tool's test fails on the result"
    files = tree(arc, arc + ".x")
    shutil.rmtree(arc + ".x", ignore_errors=True)
    for path, h in base.items():
        if path in expect_missing:
            if path in files:
                return f"{name}: {path} still there"
            continue
        if expect_moved and path == expect_moved[0]:
            if files.get(expect_moved[1]) != h:
                return f"{name}: renamed {path} -> {expect_moved[1]} missing or different"
            continue
        if files.get(path) != h:
            return f"{name}: {path} {'missing' if path not in files else 'changed'}"
    if expect_added and files.get(expect_added[0]) != expect_added[1]:
        return f"{name}: added {expect_added[0]} missing or different"
    return None


def one(src):
    base_name = os.path.basename(src)
    d = os.path.join(WORK, base_name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    k = kind(src)
    ext = ".tar.gz" if base_name.lower().endswith(".tar.gz") else os.path.splitext(base_name)[1].lower()
    rc, info = sh([AE, src, "INFO"])
    if rc != 0:
        if k and test_ok(src):
            return ("refused", f"{base_name}: {info.strip()}")
        return ("skipped", f"{base_name}: not an archive arcedit or the tools read")
    caps = info.split(":", 1)[1].split()
    if not test_ok(src):
        return ("skipped", f"{base_name}: the reference tool's test fails on the original (damaged)")
    base = tree(src, os.path.join(d, "base"))
    if not base:
        return ("skipped", f"{base_name}: no files")
    files = sorted(base)
    problems = []

    def run(op, args):
        a = os.path.join(d, op + ext)
        shutil.copy(src, a)
        rc, out = sh([AE, a] + args + ["QUIET"])
        return a, rc, out

    first = files[0]
    if "COMMENT" in caps:
        a, rc, out = run("comment", [raw(first), "COMMENT", "corpus note"])
        problems.append(f"comment: rc {rc} {out.strip()[-200:]}" if rc else check("comment", a, base))
    if "PROTECT" in caps:
        a, rc, out = run("protect", [raw(first), "PROTECT", "+s"])
        problems.append(f"protect: rc {rc} {out.strip()[-200:]}" if rc else check("protect", a, base))
    if "RENAME" in caps and k != "gz":
        new = "renamed-by-arcedit/" + os.path.basename(first)
        a, rc, out = run("rename", ["RENAME", raw(first), "AS", raw(new)])
        problems.append(f"rename: rc {rc} {out.strip()[-200:]}" if rc else check("rename", a, base, expect_moved=(first, new)))
    if "DELETE" in caps and len(files) > 1:
        gone = files[1::2]
        lst = os.path.join(d, "list")
        with open(lst, "wb") as fh:
            fh.write(b"\n".join(raw(n) for n in gone) + b"\n")
        a, rc, out = run("delete", ["DELETE", "FROM", lst])
        if not (rc == 10 and "merged" in out):   # LZX merged groups: refused by design
            problems.append(f"delete: rc {rc} {out.strip()[-200:]}" if rc else check("delete", a, base, expect_missing=set(gone)))
    if "ADD" in caps and k != "gz":
        a, rc, out = run("add", ["ADD", ADDFILE, "TO", "added-by-arcedit"])
        h = hashlib.sha1(open(ADDFILE, "rb").read()).hexdigest()
        problems.append(f"add: rc {rc} {out.strip()[-200:]}" if rc else
                        check("add", a, base, expect_added=("added-by-arcedit/" + os.path.basename(ADDFILE), h)))
    problems = [p for p in problems if p]
    shutil.rmtree(d, ignore_errors=True)
    if problems:
        return ("failed", f"{base_name} ({info.strip()}): " + "; ".join(problems))
    return ("ok", f"{base_name}: {info.strip()}, {len(files)} files")


def main():
    src = sys.argv[1]
    archives = sorted(os.path.join(src, f) for f in os.listdir(src))
    os.makedirs(WORK, exist_ok=True)
    results = {"ok": [], "failed": [], "refused": [], "skipped": []}
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for kd, text in ex.map(one, archives):
            results[kd].append(text)
            if kd != "ok":
                print(f"{kd.upper()}: {text}", flush=True)
    print(f"corpus: {len(archives)} archives: {len(results['ok'])} ok, {len(results['failed'])} failed, "
          f"{len(results['refused'])} refused, {len(results['skipped'])} skipped")
    sys.exit(1 if results["failed"] or results["refused"] else 0)


if __name__ == "__main__":
    main()

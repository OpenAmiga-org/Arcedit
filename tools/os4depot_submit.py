#!/usr/bin/env python3
"""Submit a new arcedit version to OS4Depot, replacing the published one
(utility/archive/arcedit.lha, OS4Depot file id 14270).

  tools/os4depot_submit.py           show what would be sent (nothing is sent)
  tools/os4depot_submit.py --send    send it (multipart POST, like the web form);
                                     OS4DEPOT_EMAIL must be set (it is sent hidden)

Run tools/make_lha.sh first: it writes dist/arcedit.lha and build/stage/arcedit.readme.
"""
import os
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ARCHIVE = os.path.join(ROOT, "dist", "arcedit.lha")
README = os.path.join(ROOT, "build", "stage", "arcedit.readme")
URL = "https://os4depot.net/index.php?function=submit&replace=14270&mode=go"  # replaces the published file

readme = open(README, encoding="latin-1").read()
head, body = readme.split("hend:\n", 1)
body = body.strip() + "\n"
meta = dict(l.split(":", 1) for l in head.splitlines() if ":" in l)
meta = {k.strip(): v.strip() for k, v in meta.items()}

fields = [
    ("f_name", meta["name"]),
    ("f_description", meta["description"]),
    ("f_version", meta["version"]),
    ("f_author", meta["author"]),
    ("f_submitter", meta["submitter"]),
    ("f_email", os.environ.get("OS4DEPOT_EMAIL", "")),
    ("f_hideemail", "on"),
    ("f_url", ""),
    ("f_donateurl", ""),
    ("f_category", meta["category"]),
    ("f_replaces", "utility/archive/arcedit.lha"),
    ("f_requirements", "AmigaOS 4.1"),
    ("f_license", meta["license"]),
    ("f_distributesubm", "selected"),
    ("f_distribute", "on"),
    ("f_passphrase", ""),
    ("f_minosversion", meta["minosversion"]),
    ("f_texttype", meta["texttype"]),
    ("f_text", body),
    ("f_comments", ""),
    ("f_submit_go", "Submit"),
]

print(f"POST {URL}")
for name, value in fields:
    shown = f"<readme body, {len(value)} chars>" if name == "f_text" else value
    print(f"  {name:17} = {shown!r}")
print(f"  {'f_userfile':17} = @{ARCHIVE} ({os.path.getsize(ARCHIVE)} bytes)")
assert len(meta["description"]) <= 50 and len(meta["name"]) <= 20

if "--send" not in sys.argv:
    print("\n(dry run: nothing sent)")
    sys.exit(0)
if not os.environ.get("OS4DEPOT_EMAIL"):
    sys.exit("not sent: OS4DEPOT_EMAIL missing")

cmd = ["curl", "-sS", "-o", "/dev/stdout", "-w", "\nHTTP %{http_code}\n", "--max-time", "600",
       "-A", "Mozilla/5.0 (compatible; os4depot-submit)"]
for name, value in fields:
    cmd += ["--form-string", f"{name}={value}"]
cmd += ["-F", f"f_userfile=@{ARCHIVE};type=application/x-lha;filename=arcedit.lha", URL]
out = subprocess.run(cmd, capture_output=True, text=True, encoding="latin-1")
open(os.path.join(ROOT, "build", "os4depot_response.html"), "w",
     encoding="latin-1").write(out.stdout)
print(out.stdout[-300:], out.stderr)

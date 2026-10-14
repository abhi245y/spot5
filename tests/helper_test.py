#!/usr/bin/env python3
# spot5 - Drives the pairing helper on the PC: the app side (control
# link) and a phone (HTTP). Build the helper for Linux first:
#   mkdir -p /tmp/h/ps5 && printf '#include <stdio.h>\n#define klog_printf printf\n' > /tmp/h/ps5/klog.h
#   M=third_party/cspot/cspot/bell/external/mdnssvc
#   gcc -w -I/tmp/h -I$M -D"sin_len=sin_zero[0]" -o /tmp/h/helper helper/mdns_helper.c $M/mdns.c $M/mdnsd.c -lpthread
# then: python3 tests/helper_test.py /tmp/h/helper
# SPDX-License-Identifier: GPL-3.0-or-later
import socket, subprocess, sys, time, urllib.request
helper = subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(0.3)
c = socket.create_connection(("127.0.0.1", 7865), timeout=3)
assert c.recv(6) == b"READY\n", "no READY"
info = b'{"status":101,"remoteName":"Test \\"PS5\\"","publicKey":"abc"}'
c.sendall(b"INFO %d\n" % len(info) + info)
time.sleep(0.5)
got = urllib.request.urlopen("http://127.0.0.1:7864/spotify_info?action=getInfo&version=2.7.1", timeout=3).read()
assert got == info, got
form = b"action=addUser&userName=me%40x.com&blob=" + b"A" * 300 + b"&clientKey=k%2B1"
req = urllib.request.Request("http://127.0.0.1:7864/spotify_info", data=form, method="POST")
reply = urllib.request.urlopen(req, timeout=3).read()
assert b'"status":101' in reply, reply
line = b""
while not line.endswith(b"\n"):
    line += c.recv(1)
assert line == b"LOGIN %d\n" % len(form), line
body = b""
while len(body) < len(form):
    body += c.recv(4096)
assert body == form
# 404 on anything else
try:
    urllib.request.urlopen("http://127.0.0.1:7864/other", timeout=3); assert False
except urllib.error.HTTPError as e:
    assert e.code == 404
# second helper refuses while the first runs
second = subprocess.run([sys.argv[1]], capture_output=True, text=True, timeout=5)
assert "already running" in second.stdout, second.stdout
c.close()
helper.wait(timeout=5)
out = helper.stdout.read()
print(out)
s = socket.socket(); assert s.connect_ex(("127.0.0.1", 7865)) != 0, "lock port still open"
print("ALL OK")

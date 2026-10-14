#!/usr/bin/env python3
# spot5 - Builds bell's X509 root bundle (ESP-IDF crt bundle format) from
# a PEM root store, so bell verifies TLS certificates (X509Bundle::init).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Format: BE16 count, then per root: BE16 subject_len, BE16 key_len,
# subject (DER), public key (DER SubjectPublicKeyInfo); sorted by subject
# because bell binary-searches it with memcmp.
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import serialization

source, target = sys.argv[1], sys.argv[2]
roots = {}
for cert in x509.load_pem_x509_certificates(open(source, "rb").read()):
    subject = cert.subject.public_bytes()
    key = cert.public_key().public_bytes(serialization.Encoding.DER,
                                         serialization.PublicFormat.SubjectPublicKeyInfo)
    roots.setdefault(subject, key)  # ponytail: one key per subject name
out = bytearray(struct.pack(">H", len(roots)))
for subject in sorted(roots):
    out += struct.pack(">HH", len(subject), len(roots[subject])) + subject + roots[subject]
open(target, "wb").write(out)
print(f"{target}: {len(roots)} roots, {len(out)} bytes")

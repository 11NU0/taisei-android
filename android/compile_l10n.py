#!/usr/bin/env python3
"""Compile Taisei .po files into a 10-l10n.zip resource package.

This is a dependency-free replacement for GNU gettext's msgfmt, so the
Android build can ship translations even when msgfmt/xgettext are not
installed on the host.

Usage:
    python compile_l10n.py <po_dir> <out_zip>

The output zip contains, for each <lang>.po:
    locale/<lang>/LC_MESSAGES/taisei.mo
"""

import os
import struct
import sys
import zipfile

MO_MAGIC = 0x950412DE


def unescape_po(s):
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "\\" and i + 1 < n:
            nxt = s[i + 1]
            mapping = {
                "n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\",
                "a": "\a", "b": "\b", "f": "\f", "v": "\v",
            }
            if nxt in mapping:
                out.append(mapping[nxt])
                i += 2
                continue
            # keep unknown escapes verbatim (matches gettext behavior)
            out.append("\\")
            out.append(nxt)
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _literal(line):
    s = line.strip()
    if s.startswith('"'):
        return s[1:-1] if s.endswith('"') else s[1:]
    return None


def parse_po(text):
    entries = []
    lines = text.split("\n")
    i = 0
    n = len(lines)

    while i < n:
        s = lines[i].strip()
        if not s or s.startswith("#"):
            i += 1
            continue

        if not s.startswith("msgid"):
            i += 1
            continue

        # msgid (possibly empty "" on the same line, plus continuation lines)
        rest = s[5:].strip()
        msgid_parts = []
        if rest.startswith('"'):
            msgid_parts.append(rest[1:-1] if rest.endswith('"') else rest[1:])
        i += 1
        while i < n:
            lit = _literal(lines[i])
            if lit is None:
                break
            msgid_parts.append(lit)
            i += 1
        msgid = unescape_po("".join(msgid_parts))

        # skip blank/comment lines, then read msgstr
        while i < n and (not lines[i].strip() or lines[i].strip().startswith("#")):
            i += 1

        msgstr_parts = []
        if i < n and lines[i].strip().startswith("msgstr"):
            rest = lines[i].strip()[6:].strip()
            if rest.startswith('"'):
                msgstr_parts.append(rest[1:-1] if rest.endswith('"') else rest[1:])
            i += 1
            while i < n:
                lit = _literal(lines[i])
                if lit is None:
                    break
                msgstr_parts.append(lit)
                i += 1
        msgstr = unescape_po("".join(msgstr_parts))

        if msgid != "":
            entries.append((msgid, msgstr))

    return entries


def build_mo(entries):
    n = len(entries)
    orig = [e[0].encode("utf-8") for e in entries]
    trans = [e[1].encode("utf-8") for e in entries]

    data = b""
    orig_off = []
    for b in orig:
        orig_off.append(len(data))
        data += b + b"\x00"
    trans_off = []
    for b in trans:
        trans_off.append(len(data))
        data += b + b"\x00"

    data_start = 28 + 16 * n

    header = struct.pack("<IIIIIII", MO_MAGIC, 0, n, 28, 28 + 8 * n, 0, 0)
    orig_desc = b"".join(
        struct.pack("<II", len(orig[i]), data_start + orig_off[i]) for i in range(n)
    )
    trans_desc = b"".join(
        struct.pack("<II", len(trans[i]), data_start + trans_off[i]) for i in range(n)
    )

    return header + orig_desc + trans_desc + data


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2

    po_dir, out_zip = argv[1], argv[2]
    po_files = sorted(
        f for f in os.listdir(po_dir) if f.endswith(".po")
    )

    if not po_files:
        print("No .po files found in %s" % po_dir, file=sys.stderr)
        return 1

    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED) as zf:
        written_dirs = set()

        def write_dir(d):
            if d not in written_dirs:
                zf.writestr(d, b"")
                written_dirs.add(d)

        for po in po_files:
            lang = po[:-3]
            with open(os.path.join(po_dir, po), "r", encoding="utf-8") as f:
                entries = parse_po(f.read())
            mo = build_mo(entries)
            arcname = "locale/%s/LC_MESSAGES/taisei.mo" % lang

            # Explicit directory entries are required by Taisei's VFS
            # zip mount to enumerate directories.
            write_dir("locale/")
            write_dir("locale/%s/" % lang)
            write_dir("locale/%s/LC_MESSAGES/" % lang)

            zf.writestr(arcname, mo)
            print("  %s -> %s (%d strings)" % (po, arcname, len(entries)))

    print("Wrote %s" % out_zip)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

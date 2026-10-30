#!/usr/bin/env python3
"""Resolve Alpine package dependencies from APKINDEX files, without apk.

Usage: apk_resolve.py <installed-db> <APKINDEX>... -- <package>...

<installed-db> is the minirootfs' lib/apk/db/installed (packages already in
the rootfs are skipped). A package given as !name is never installed, even
as a dependency (e.g. large data packages an app can run without).

Prints one "<repo-index-path> <name> <version>" per package to install,
dependencies first. Exits non-zero on unresolvable deps.
"""

import sys


def parse(path):
    """Yield one dict per package record (single-letter keys)."""
    rec = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if not line:
            if rec:
                yield rec
            rec = {}
            continue
        if line[1:2] == ":":
            rec.setdefault(line[0], []).append(line[2:])
    if rec:
        yield rec


def dep_name(dep):
    """'so:libfoo.so.1', 'foo>=1.2', 'foo=1' -> provider key."""
    for op in ("<", ">", "=", "~"):
        dep = dep.split(op, 1)[0]
    return dep


def main():
    args = sys.argv[1:]
    sep = args.index("--")
    installed_db, indexes, wanted = args[0], args[1:sep], args[sep + 1:]

    installed = set()
    for rec in parse(installed_db):
        installed.add(rec["P"][0])
        for p in rec.get("p", [""])[0].split():
            installed.add(dep_name(p))

    pkgs, provides = {}, {}
    for idx in indexes:
        for rec in parse(idx):
            name = rec["P"][0]
            pkgs.setdefault(name, (idx, rec))
            for p in [name] + rec.get("p", [""])[0].split():
                provides.setdefault(dep_name(p), name)

    order, seen = [], set()

    def visit(key, why):
        if key in installed or key.startswith("!"):
            return
        name = provides.get(key)
        if name is None:
            sys.exit(f"unresolvable: {key} (needed by {why})")
        if name in seen or name in installed:
            return
        seen.add(name)
        idx, rec = pkgs[name]
        for d in rec.get("D", [""])[0].split():
            visit(dep_name(d), name)
        order.append((idx, name, rec["V"][0]))

    for w in wanted:
        if w.startswith("!"):
            installed.add(w[1:])
    for w in wanted:
        if not w.startswith("!"):
            visit(w, "build/packages")
    for idx, name, ver in order:
        print(idx, name, ver)


if __name__ == "__main__":
    main()

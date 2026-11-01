"""Checks a whole database file: header, schema tree, every table's B+ tree, free list.
Usage: python check_db.py <db file> [<table> <file with expected sorted ids>]..."""
import struct, sys

PAGE = 4096
ROW = 293
COMMON = 6
LEAF_HDR, LEAF_CELL = COMMON + 8, 4 + ROW
INT_HDR, INT_CELL = COMMON + 8, 8
LEAF_MAX, INT_MAX = (PAGE - LEAF_HDR) // LEAF_CELL, (PAGE - INT_HDR) // INT_CELL
LEAF_MIN, INT_MIN_CHILDREN = LEAF_MAX // 2, (INT_MAX + 1) // 2

def u32(p, off):
    return struct.unpack_from("<I", p, off)[0]

def varint(b, pos):
    v = 0
    for i in range(9):
        if i == 8:
            return (v << 8) | b[pos + i], pos + 9
        v = (v << 7) | (b[pos + i] & 0x7F)
        if not b[pos + i] & 0x80:
            return v, pos + i + 1

def body_size(t):
    return {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 6, 6: 8, 7: 8, 8: 0, 9: 0}.get(t, (t - 12) // 2 if t >= 12 else None)

def decode_record(b):
    hsize, pos = varint(b, 0)
    types = []
    while pos < hsize:
        t, pos = varint(b, pos)
        types.append(t)
    if pos != hsize:
        raise ValueError("header overruns its size")
    vals, body = [], hsize
    for t in types:
        n = body_size(t)
        if n is None:
            raise ValueError(f"bad serial type {t}")
        raw = b[body:body + n]
        if len(raw) != n:
            raise ValueError("body runs past record")
        if t == 0: vals.append(None)
        elif t in (8, 9): vals.append(t - 8)
        elif t <= 6: vals.append(int.from_bytes(raw, "big", signed=True))
        elif t == 7: vals.append(struct.unpack(">d", raw)[0])
        elif t % 2 == 0: vals.append(bytes(raw))
        else: vals.append(raw.decode("utf-8", "replace"))
        body += n
    if body != len(b):
        raise ValueError("extra bytes after body")
    return vals, types

def cell_record(p, off):
    size = (p[off] << 8) | p[off + 1]
    if size > ROW - 2:
        raise ValueError(f"record length {size} > {ROW - 2}")
    return decode_record(p[off + 2:off + 2 + size])

def check(path, expected=None):
    data = open(path, "rb").read()
    errors = []
    if len(data) % PAGE:
        return ["file is not whole pages"], {}
    num_pages = len(data) // PAGE
    page = lambda n: data[n * PAGE:(n + 1) * PAGE]
    if page(0)[:8] != b"MyDB v1\0":
        errors.append("bad magic")
    if u32(page(0), 8) != PAGE:
        errors.append("bad page size in header")

    owner = {0: "header"}

    def walk_tree(name, root):
        leaves, depths, internal = [], set(), [0]
        def walk(n, parent, depth, low, high):
            if not 0 < n < num_pages:
                errors.append(f"{name}: page {n} out of range, parent {parent}")
                return None
            if n in owner:
                errors.append(f"{name}: page {n} already used by {owner[n]}")
                return None
            owner[n] = name
            p = page(n)
            if bool(p[1]) != (n == root):
                errors.append(f"{name}: page {n} is_root={p[1]}")
            if n != root and u32(p, 2) != parent:
                errors.append(f"{name}: page {n} parent {u32(p, 2)}, expected {parent}")
            if p[0] == 1:
                cells = u32(p, COMMON)
                if cells > LEAF_MAX or (n != root and cells < LEAF_MIN):
                    errors.append(f"{name}: leaf {n} has {cells} cells")
                keys, rows = [], []
                for i in range(cells):
                    k = u32(p, LEAF_HDR + i * LEAF_CELL)
                    if keys and keys[-1] >= k:
                        errors.append(f"{name}: leaf {n} keys not sorted")
                    if (low is not None and k <= low) or (high is not None and k > high):
                        errors.append(f"{name}: leaf {n} key {k} outside ({low}, {high}]")
                    try:
                        rows.append((k, cell_record(p, LEAF_HDR + i * LEAF_CELL + 4)[0]))
                    except Exception as e:
                        errors.append(f"{name}: leaf {n} key {k}: bad record: {e}")
                    keys.append(k)
                leaves.append((n, rows))
                depths.add(depth)
                return (keys[0], keys[-1]) if keys else (None, None)
            if p[0] != 0:
                errors.append(f"{name}: page {n} has node type {p[0]} inside the tree")
                return None
            internal[0] += 1
            nk = u32(p, COMMON)
            if nk == 0 or nk > INT_MAX or (n != root and nk + 1 < INT_MIN_CHILDREN):
                errors.append(f"{name}: internal {n} has {nk} keys")
            children = [u32(p, INT_HDR + i * INT_CELL) for i in range(nk)] + [u32(p, COMMON + 4)]
            keys = [u32(p, INT_HDR + i * INT_CELL + 4) for i in range(nk)]
            if keys != sorted(set(keys)):
                errors.append(f"{name}: internal {n} keys not sorted")
            lo, rmin, rmax = low, None, None
            for i, c in enumerate(children):
                r = walk(c, n, depth + 1, lo, keys[i] if i < nk else high)
                if r is None:
                    continue
                if i < nk and r[1] != keys[i]:
                    errors.append(f"{name}: internal {n} key[{i}]={keys[i]} but child max is {r[1]}")
                rmin = r[0] if rmin is None else rmin
                rmax = r[1]
                if i < nk:
                    lo = keys[i]
            return rmin, rmax
        walk(root, None, 0, None, None)
        if len(depths) > 1:
            errors.append(f"{name}: leaves at different depths {sorted(depths)}")
        chain, n = [], leaves[0][0] if leaves else None
        while n is not None:
            chain.append(n)
            nxt = u32(page(n), COMMON + 4)
            n = nxt if nxt != 0 else None
            if len(chain) > num_pages:
                errors.append(f"{name}: next_leaf loop")
                break
        if chain != [l[0] for l in leaves]:
            errors.append(f"{name}: next_leaf chain doesn't match tree order")
        rows = [r for _, rs in leaves for r in rs]
        return rows, max(depths) + 1 if depths else 0, internal[0], len(leaves)

    summary = {}
    schema_rows, *_ = walk_tree("schema", 1)
    for key, vals in schema_rows:
        if len(vals) != 5 or vals[0] != "table" or not isinstance(vals[3], int):
            errors.append(f"schema row {key} looks wrong: {vals}")
            continue
        rows, levels, internal, leaves = walk_tree(vals[1], vals[3])
        summary[vals[1]] = rows
        print(f"   table {vals[1]}: root={vals[3]} rows={len(rows)} levels={levels} internal={internal} leaves={leaves}")

    free, f = [], u32(page(0), 12)
    while f:
        if not 0 < f < num_pages or f in owner:
            errors.append(f"free list hits page {f} ({owner.get(f, 'out of range')})")
            break
        if page(f)[0] != 2:
            errors.append(f"free page {f} has type {page(f)[0]}")
        owner[f] = "free"
        free.append(f)
        f = u32(page(f), 2)
    if len(owner) != num_pages:
        lost = sorted(set(range(num_pages)) - set(owner))
        errors.append(f"{len(lost)} pages belong to nothing, e.g. {lost[:5]}")
    print(f"   pages={num_pages} free={len(free)} tables={len(summary)}")

    for table, want in (expected or {}).items():
        got = [k for k, _ in summary.get(table, [])]
        if got != want:
            errors.append(f"{table}: ids differ from expected ({len(got)} vs {len(want)})")
    return errors, summary

if __name__ == "__main__":
    exp = {}
    for i in range(2, len(sys.argv), 2):
        exp[sys.argv[i]] = [int(x) for x in open(sys.argv[i + 1]).read().split()]
    errs, _ = check(sys.argv[1], exp)
    print("OK" if not errs else f"{len(errs)} ERRORS")
    for e in errs[:15]:
        print("  ", e)
    sys.exit(1 if errs else 0)

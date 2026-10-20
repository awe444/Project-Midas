#!/usr/bin/env python3
"""Compile one asset TU with clang, keeping its globals in source order.

The compiled-in asset objects (CMakeLists.txt, "Compiled-in assets") must lay
.data out byte-for-byte like the N64 file, because load_object_fill_header
finds Textures/RootNode by offset arithmetic. gcc gets that from
-fno-toplevel-reorder. Clang has no such switch: it creates each global the
first time anything mentions it (the generated Model.c files forward-declare
everything as extern and cross-reference node to node), and emits them in that
order. NDK clang 18 rejects the flag outright.

LLVM does emit globals in module order, and a .ll file lists one global
definition per line, so this compiles to IR, puts the definition lines back
in the order the preprocessed source defines them, and compiles the IR.

With --check-offsets it then reads the object's symbols with llvm-nm and fails
if any symbol named *_0x<hex> (the generators' convention: the N64 file offset)
sits anywhere else - so a layout regression breaks the build, not the game.

Usage:
  asset_cc_ordered.py --cc CLANG [--nm LLVM_NM] [--check-offsets] \
      -o OUT.o SRC.c -- CFLAGS...
"""
import argparse
import os
import re
import subprocess
import sys

IR_DEF = re.compile(r'^@([A-Za-z_.$][\w.$]*) = (?!external\b)')
IDENT = re.compile(r'[A-Za-z_]\w*')
OFFSET_NAME = re.compile(r'_0x([0-9a-fA-F]+)$')
SKIP_LEAD = {'extern', 'typedef', 'struct', 'union', 'enum'}


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit(r.returncode)
    return r.stdout


def source_definition_order(text):
    """Names of the file-scope object definitions, in definition order.

    Works on preprocessed text: walks top-level statements (brace depth 0)
    and takes the declarator name of each one that is not extern/typedef/a
    bare tag/a function. Good enough for the generated asset C, which is all
    plain `Type name[...] = {...};` definitions."""
    order = []
    depth = 0
    stmt = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            stmt.append(text[i:j + 1])
            i = j + 1
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                # a function body ends a statement without ';'
                head = ''.join(stmt)
                if '(' in head.split('=')[0]:
                    stmt = []
                    i += 1
                    continue
        if c == ';' and depth == 0:
            name = declarator_name(''.join(stmt))
            if name is not None and name not in order:
                order.append(name)
            stmt = []
        else:
            stmt.append(c)
        i += 1
    return order


def declarator_name(stmt):
    stmt = stmt.strip()
    if not stmt or stmt.startswith('#'):
        return None
    head = stmt.split('=', 1)[0]
    # strip attributes before looking at the declarator
    head = re.sub(r'__attribute__\s*\(\(.*?\)\)', ' ', head, flags=re.S)
    words = IDENT.findall(head)
    if not words or words[0] in ('extern', 'typedef'):
        return None
    if '=' not in stmt and words[0] in SKIP_LEAD and '{' in stmt:
        return None  # a bare struct/union/enum definition
    if '(' in head:
        return None  # prototype (or function-pointer object: rare in assets)
    # the declarator name is the last identifier before any [dimensions]
    decl = head.split('[', 1)[0]
    names = IDENT.findall(decl)
    return names[-1] if names else None


def reorder_ir(ir_text, order):
    lines = ir_text.split('\n')
    slots = [i for i, l in enumerate(lines) if IR_DEF.match(l)]
    rank = {name: k for k, name in enumerate(order)}
    defs = [lines[i] for i in slots]
    # unknown names keep their relative IR order, after the known ones
    keyed = sorted(enumerate(defs),
                   key=lambda kv: (rank.get(IR_DEF.match(kv[1]).group(1), len(rank)), kv[0]))
    for slot, (_, line) in zip(slots, keyed):
        lines[slot] = line
    return '\n'.join(lines)


def check_offsets(nm, obj):
    out = run([nm, '--defined-only', obj])
    bad = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in 'dDbB':
            continue
        m = OFFSET_NAME.search(parts[2])
        if m and int(m.group(1), 16) != int(parts[0], 16):
            bad.append('%s at 0x%x' % (parts[2], int(parts[0], 16)))
    if bad:
        sys.stderr.write('%s: %d symbol(s) not at their N64 offset: %s\n'
                         % (obj, len(bad), ', '.join(bad[:8])))
        sys.exit(1)


def main():
    argv = sys.argv[1:]
    if '--' not in argv:
        sys.exit('asset_cc_ordered.py: missing -- before the compiler flags')
    split = argv.index('--')
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', required=True)
    ap.add_argument('--nm')
    ap.add_argument('--check-offsets', action='store_true')
    ap.add_argument('-o', dest='out', required=True)
    ap.add_argument('src')
    args = ap.parse_args(argv[:split])
    cflags = argv[split + 1:]

    pre = run([args.cc] + cflags + ['-E', '-P', args.src])
    ir_path = args.out + '.ll'
    run([args.cc] + cflags + ['-S', '-emit-llvm', args.src, '-o', ir_path])
    with open(ir_path) as f:
        ir = f.read()
    with open(ir_path, 'w') as f:
        f.write(reorder_ir(ir, source_definition_order(pre)))
    # Only the target and codegen flags apply to .ll input (-w: clang warns
    # about the C-only rest). -fno-zero-initialized-in-bss is codegen: without
    # it here every zero PADDING_* word moves to .bss and shifts the layout.
    codegen = ('--target', '-target', '-march', '-mthumb', '-marm', '-mfloat', '-mfpu', '-O',
               '-fno-zero-initialized-in-bss', '-fno-common', '-fno-pic', '-fPIC', '-fpic', '--sysroot')
    run([args.cc] + [f for f in cflags if f.startswith(codegen)]
        + ['-w', '-c', ir_path, '-o', args.out])
    os.remove(ir_path)
    if args.check_offsets:
        if not args.nm:
            sys.exit('asset_cc_ordered.py: --check-offsets needs --nm')
        check_offsets(args.nm, args.out)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Surgical text replacement for the C++ sources.

The sources carry a mix of line endings and some of the anchors below span several lines, so a
straight string replace on a whole file is the wrong tool: it is silent about how many places it
hit, and it can rewrite the newline style of text that was never meant to change. This helper
reads the file as bytes, decodes it, asserts the anchor occurs exactly once, and writes back with
the newline style it found. Anything else aborts without touching the file.

    python tools/patch_helper.py <file> <old-file> <new-file>

where <old-file> and <new-file> hold the literal anchor and its replacement.
"""
import sys


def read_text(path):
    with open(path, 'rb') as f:
        raw = f.read()
    text = raw.decode('utf-8')
    crlf = text.count('\r\n')
    bare_cr = text.count('\r') - crlf
    lf = text.count('\n') - crlf
    if crlf and bare_cr:
        raise SystemExit('%s: mixed CR and CRLF endings, refusing to guess' % path)
    style = '\r\n' if crlf else '\n'
    if bare_cr:
        style = '\r'
    # A file with no line breaks at all reports as LF; nothing to normalise in that case anyway.
    del lf
    return text.replace('\r\n', '\n').replace('\r', '\n'), style


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    target, old_path, new_path = sys.argv[1], sys.argv[2], sys.argv[3]
    text, style = read_text(target)
    old, _ = read_text(old_path)
    new, _ = read_text(new_path)
    # The patch fragments are written with the same endings as the source they belong to, and a
    # trailing newline on the fragment is a property of the editor that saved it, not of the text.
    old = old.rstrip('\n')
    new = new.rstrip('\n')
    n = text.count(old)
    if n != 1:
        raise SystemExit('anchor found %d times in %s (want exactly 1) - nothing written' % (n, target))
    out = text.replace(old, new, 1)
    with open(target, 'wb') as f:
        f.write(out.replace('\n', style).encode('utf-8'))
    print('patched %s (%d bytes -> %d bytes, %s endings)'
          % (target, len(text), len(out), 'CRLF' if style == '\r\n' else 'LF'))


if __name__ == '__main__':
    main()

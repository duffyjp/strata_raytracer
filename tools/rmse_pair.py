"""RMSE between two capture directories' first frame.

Used to test whether consecutive accumulation frames are independent. Averaging two
independent noisy images gives img1 - img2 = (m2 - m1) / 2, so with per-sample error sigma
the expected difference is sigma / sqrt(2). A result near zero means the two frames are the
same sample, not two independent ones.
"""
import os, sys, zlib, struct


def load(path):
    d = open(path, 'rb').read()
    i, w, h, idat = 8, 0, 0, b''
    while i < len(d):
        ln = struct.unpack('>I', d[i:i + 4])[0]
        typ = d[i + 4:i + 8]
        data = d[i + 8:i + 8 + ln]
        if typ == b'IHDR':
            w, h = struct.unpack('>II', data[:8])
        elif typ == b'IDAT':
            idat += data
        elif typ == b'IEND':
            break
        i += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 3
    px = bytearray(w * h * bpp)
    prev = bytearray(w * bpp)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + w * bpp]); p += w * bpp
        for x in range(w * bpp):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        px[y * w * bpp:(y + 1) * w * bpp] = line
        prev = line
    return w, h, px


def first(d):
    return os.path.join(d, [x for x in sorted(os.listdir(d)) if x.endswith('.png')][0])


def rmse(pa, pb):
    _, _, a = load(pa)
    _, _, b = load(pb)
    tot, n = 0.0, 0
    for o in range(0, min(len(a), len(b)) - 2, 3):
        dr = a[o] - b[o]
        dg = a[o + 1] - b[o + 1]
        db = a[o + 2] - b[o + 2]
        tot += (dr * dr + dg * dg + db * db) / 3.0
        n += 1
    return (tot / n) ** 0.5


if __name__ == '__main__':
    argv = sys.argv[1:]
    for k in range(0, len(argv) - 1, 2):
        x, y = argv[k], argv[k + 1]
        print("  %-10s vs %-10s  RMSE %.2f" % (os.path.basename(x), os.path.basename(y),
                                               rmse(first(x), first(y))))

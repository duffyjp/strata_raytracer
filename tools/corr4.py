"""Noise independence of accumulation frames, from a single capture run.

Reads the raw float dumps written by `--capture --dump-accum -2`. The accumulation target is a
running sum, so frame k's own sample is s_k - s_k-1. All statistics come from one process, because
separate runs of this renderer do not reproduce the same frame sequence.

For independent frames the variance of a frame-to-frame difference is the same at every lag. If
consecutive frames repeat each other's noise, short lags dip below long lags.
"""
import math
import struct
import sys

N = 8


def load(d, k):
    path = '%s\\accum_%03d.bin' % (d, k)
    b = open(path, 'rb').read()
    w, h = struct.unpack('<II', b[:8])
    return w, h, struct.unpack('<%df' % (w * h * 4), b[8:])


def var(a):
    m = sum(a) / len(a)
    return sum((v - m) ** 2 for v in a) / (len(a) - 1)


def pear(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    num = da = db = 0.0
    for i in range(len(a)):
        x, y = a[i] - ma, b[i] - mb
        num += x * y
        da += x * x
        db += y * y
    return num / math.sqrt(da * db + 1e-30)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'one8'
    w, h, s0 = load(d, 0)
    # The target is a running sum of per-frame means, so a frame's own sample is the increment
    # over the previous dump. Frame 0 has no predecessor.
    raw = []
    for k in range(N):
        _, _, s = load(d, k)
        raw.append(list(s))
    M = [raw[0]] + [[raw[k][i] - raw[k - 1][i] for i in range(len(raw[0]))] for k in range(1, N)]

    y0, y1 = int(h * 0.72), int(h * 0.90)
    pix = [(y * w + x) * 4 + c for y in range(y0, y1) for x in range(2, w - 2) for c in range(3)]
    V = [[M[k][p] for p in pix] for k in range(N)]
    print("%s: %d pixel samples in the floor band, %d frames from one process" % (d, len(pix), N))
    print("  per-frame sample mean: %s" % ", ".join("%.3f" % (sum(v) / len(v)) for v in V))
    print()
    print("variance of the difference between two frames, by lag.")
    print("independent frames: constant. repeating noise: short lags dip.\n")
    for lag in range(1, N):
        vals = []
        for i in range(N - lag):
            vals.extend(V[i][j] - V[i + lag][j] for j in range(len(pix)))
        print("   lag %d   var(diff) = %.4f" % (lag, var(vals)))
    print()
    # single-frame noise variance, averaged over all distinct pairs: Var(a-b) = Var(a)+Var(b) - 2Cov
    diffs = []
    for a in range(N):
        for b in range(a + 1, N):
            diffs.append(var([V[a][j] - V[b][j] for j in range(len(pix))]))
    sig2 = sum(diffs) / len(diffs) / 2.0
    # split the eight frames into two halves; if the four in each half were independent, the
    # difference between the half-means would have variance 2*sig2/4
    even = [[sum(V[k][j] for k in (0, 2, 4, 6)) / 4.0 for j in range(len(pix))]]
    odd = [[sum(V[k][j] for k in (1, 3, 5, 7)) / 4.0 for j in range(len(pix))]]
    dv = var([even[0][j] - odd[0][j] for j in range(len(pix))])
    print("single-frame noise variance from all pairs : %.4f" % sig2)
    print("variance between the means of two halves   : %.4f" % dv)
    print("independent frames would give 2*sig2/4      = %.4f" % (2 * sig2 / 4.0))
    if dv > 0:
        print("=> effective independent samples per group of four: %.2f" % (2 * sig2 / dv))
    print()
    print("disjoint-difference correlation (four different frames, no shared term):")
    for a, b in [(0, 2), (0, 4), (1, 5)]:
        da = [V[a][j] - V[a + 1][j] for j in range(len(pix))]
        db = [V[b][j] - V[b + 1][j] for j in range(len(pix))]
        print("   corr(frame%d-frame%d, frame%d-frame%d) = %+.4f" % (a, a + 1, b, b + 1, pear(da, db)))


if __name__ == '__main__':
    main()

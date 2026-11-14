#!/usr/bin/env python3
"""imgcmp.py - compare two PNGs with the criteria from browser/README.md section 9.3, decoded
outside the browser.

Why this exists next to web/png_check.py: png_check answers "is there a picture, and is the picture
on screen the picture that was rendered". It does not answer "how close is this render to that
render", which is the parity question. This does, and it decodes with png_check.decode() - the
standard-library PNG reader (zlib for IDAT, the five scanline filters by hand, no PIL) - so the
numbers are produced by code that shares nothing with the browser encoder that wrote one of the
files, and share nothing with imageio.js, which produced the same numbers inside the page.

The criteria are the ones fixed in README section 9.3 BEFORE the new app was written, copied here
rather than invented here:

    G1 depth    within 4/255  >= 99.0 %       sky/no-feature mask agreement >= 98.0 %
    G2 albedo   within 12/255 >= 95 %, mean |diff| <= 6
    G3 normal   within 12/255 >= 95 %
    S1 floor band mean luminance within 6 %
    S2 whole frame within 16/255 >= 70 %
    S3 floor band within 16/255 >= 85 %
    S4 whole-frame luminance within 8 %

The floor band is the same rectangle the previous attempt used: x 0.30-0.70, y 0.84-0.96.

    C:\\Python315\\python.exe build\\html\\tools\\imgcmp.py A.png B.png [--tol 16] [--channel still]

Exit status 0 when every criterion the chosen channel defines is met, 1 otherwise.
"""
import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
# browser/tools/imgcmp.py -> browser -> the repository root, and web/ is under that.
_REPO = os.path.dirname(os.path.dirname(_HERE))
sys.path.insert(0, os.path.join(_REPO, 'web'))
from png_check import decode                      # noqa: E402  (the independent decoder)

LUMA = (0.2126, 0.7152, 0.0722)
BAND = (0.30, 0.70, 0.84, 0.96)                   # x0 x1 y0 y1, as fractions of the frame
CRITERIA = {
    'depth': [('within', 4, 0.99), ('sky', 2, 0.98)],
    'albedo': [('within', 12, 0.95), ('mean', 12, 6.0)],
    'normal': [('within', 12, 0.95)],
    'still': [('bandrel', None, 0.06), ('within', 16, 0.70), ('bandwithin', 16, 0.85),
              ('framerel', None, 0.08)],
}


def luma(px, p, nch):
    i = p * nch
    return px[i] * LUMA[0] + px[i + 1] * LUMA[1] + px[i + 2] * LUMA[2]


def stats(a, b, tol):
    """The same statistics imageio.js compareImages() computes in the page."""
    (w, h, nch, pa), (_, _, nch2, pb) = a, b
    n = w * h
    tot = worst = worst_i = within = 0
    tot_max = 0
    whole_a = whole_b = 0.0
    for p in range(n):
        d = 0
        for c in range(3):
            diff = abs(pa[p * nch + c] - pb[p * nch2 + c])
            d = max(d, diff)
            tot += diff
        tot_max += d
        if d > worst:
            worst, worst_i = d, p
        if d <= tol:
            within += 1
        whole_a += pa[p * nch] * LUMA[0] + pa[p * nch + 1] * LUMA[1] + pa[p * nch + 2] * LUMA[2]
        whole_b += pb[p * nch2] * LUMA[0] + pb[p * nch2 + 1] * LUMA[1] + pb[p * nch2 + 2] * LUMA[2]
    x0, x1, y0, y1 = BAND
    bx0, bx1, by0, by1 = int(w * x0), int(w * x1), int(h * y0), int(h * y1)
    band_a = band_b = 0.0
    band_within = band_tot = 0
    for y in range(by0, by1):
        for x in range(bx0, bx1):
            p = y * w + x
            band_a += luma(pa, p, nch)
            band_b += luma(pb, p, nch2)
            d = max(abs(pa[p * nch + c] - pb[p * nch2 + c]) for c in range(3))
            if d <= tol:
                band_within += 1
            band_tot += 1
    band_a /= max(band_tot, 1)
    band_b /= max(band_tot, 1)
    whole_a /= n
    whole_b /= n
    return {
        'pixels': n,
        'mean_abs_diff': tot / (n * 3),
        # The page's own statistic (imageio.js compareImages sums the worst of the three channel
        # differences per pixel), kept under this name so the two are never confused.
        'mean_max_abs_diff': tot_max / n,
        f'within_{tol}': within / n,
        'worst': worst,
        'worst_at': f'{worst_i % w},{worst_i // w}',
        'band_luma_a': band_a,
        'band_luma_b': band_b,
        'band_rel_luma': abs(band_a - band_b) / max(band_b, 1e-9),
        'band_within': band_within / max(band_tot, 1),
        'frame_luma_a': whole_a,
        'frame_luma_b': whole_b,
        'frame_rel_luma': abs(whole_a - whole_b) / max(whole_b, 1e-9),
    }


def sky_mask(px, nch, w, h, code=2):
    return [1 if (px[p * nch] <= code and px[p * nch + 1] <= code
                  and px[p * nch + 2] <= code) else 0 for p in range(w * h)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--tol', type=int, default=None,
                    help='override the channel tolerance (default: the one README 9.3 fixes for it)')
    ap.add_argument('--channel', default=None,
                    choices=sorted(CRITERIA), help='which criteria to judge by (default: from the name)')
    args = ap.parse_args()

    a = decode(args.a)
    b = decode(args.b)
    chan = args.channel
    if chan is None:
        names = (os.path.basename(args.a) + os.path.basename(args.b)).lower()
        for k in ('depth', 'albedo', 'normal'):
            if k in names:
                chan = k
                break
        chan = chan or 'still'
    tol = args.tol if args.tol is not None else {'depth': 4, 'albedo': 12, 'normal': 12,
                                                 'still': 16}[chan]
    s = stats(a, b, tol)
    print(f'{os.path.basename(args.a)} vs {os.path.basename(args.b)}  channel={chan}  '
          f'{a[0]}x{a[1]} vs {b[0]}x{b[1]}')
    for k in sorted(s):
        print(f'   {k:16} {s[k] if not isinstance(s[k], float) else round(s[k], 4)}')
    failures = []
    for kind, tol, limit in CRITERIA[chan]:
        if kind == 'within':
            ok = s[f'within_{tol}'] >= limit
            failures.append((f'within {tol}/255 >= {limit}', ok, f"{s[f'within_{tol}']:.4f}"))
        elif kind == 'mean':
            # The page's own mean is the mean over pixels of the worst-channel difference (imageio.js
            # compareImages: sumDiff += max of the three channel differences), and the recorded baseline
            # was written with that definition - so the criterion is judged on the same statistic, with
            # the per-channel one printed alongside it.
            ok = s['mean_max_abs_diff'] <= limit
            failures.append((f'mean worst-channel |diff| <= {limit}', ok,
                             f"{s['mean_max_abs_diff']:.2f} (per channel {s['mean_abs_diff']:.2f})"))
        elif kind == 'bandrel':
            ok = s['band_rel_luma'] <= limit
            failures.append((f'floor band luminance within {limit:.0%}', ok,
                             f"{s['band_rel_luma']:.4f}"))
        elif kind == 'bandwithin':
            ok = s['band_within'] >= limit
            failures.append((f'floor band within {tol}/255 >= {limit}', ok,
                             f"{s['band_within']:.4f}"))
        elif kind == 'framerel':
            ok = s['frame_rel_luma'] <= limit
            failures.append((f'whole-frame luminance within {limit:.0%}', ok,
                             f"{s['frame_rel_luma']:.4f}"))
        elif kind == 'sky':
            ma = sky_mask(a[3], a[2], a[0], a[1])
            mb = sky_mask(b[3], b[2], b[0], b[1])
            agree = sum(1 for x, y in zip(ma, mb) if x == y) / len(ma)
            failures.append((f'no-feature-surface mask agreement >= {limit}', agree >= limit,
                             f'{agree:.4f}'))
    for name, ok, val in failures:
        print(f'   {"PASS" if ok else "FAIL"}  {name}   ({val})')
    bad = [f for f in failures if not f[1]]
    print(f'IMGCMP {"PASS" if not bad else "FAIL"}  channel={chan}  failing={len(bad)}')
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())

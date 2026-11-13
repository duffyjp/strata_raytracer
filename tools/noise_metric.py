import zlib, struct, os, math
def load(path):
    d = open(path, 'rb').read(); i = 8; w = h = 0; idat = b''
    while i < len(d):
        ln = struct.unpack('>I', d[i:i+4])[0]; typ = d[i+4:i+8]; data = d[i+8:i+8+ln]
        if typ == b'IHDR': w, h = struct.unpack('>II', data[:8])
        elif typ == b'IDAT': idat += data
        elif typ == b'IEND': break
        i += 12 + ln
    raw = zlib.decompress(idat); bpp = 3; out = bytearray(w*h*bpp); prev = bytearray(w*bpp)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1; line = bytearray(raw[p:p+w*bpp]); p += w*bpp
        for x in range(w*bpp):
            a = line[x-bpp] if x >= bpp else 0; b = prev[x]; c = prev[x-bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x]+a) & 255
            elif f == 2: line[x] = (line[x]+b) & 255
            elif f == 3: line[x] = (line[x]+(a+b)//2) & 255
            elif f == 4:
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x]+pr) & 255
        out[y*w*bpp:(y+1)*w*bpp] = line; prev = line
    return w, h, out
def noise(path, y0, y1):
    w, h, px = load(path)
    # mean absolute deviation from a 5x5 box blur over a floor band: pure high-frequency energy
    tot = 0.0; n = 0
    for y in range(y0, y1, 2):
        for x in range(8, w-8, 2):
            s = 0.0; c = 0
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    o = ((y+dy)*w + (x+dx))*3
                    s += px[o] + px[o+1] + px[o+2]; c += 1
            m = s/(c*3.0); o = (y*w+x)*3
            v = (abs(px[o]-m)+abs(px[o+1]-m)+abs(px[o+2]-m))/3.0
            tot += v; n += 1
    return tot/n
def mean(path):
    w, h, px = load(path)
    tot = 0.0
    for o in range(0, len(px), 9):
        tot += (px[o] + px[o+1] + px[o+2]) / 3.0
    return tot / (len(px) / 9.0)
for name in ['cmp_raw','cmp_prog','cmp_filt']:
    f = [x for x in os.listdir(name) if x.endswith('.png')][0]
    fp = os.path.join(name, f)
    print("%-10s noise %.3f | mean luminance %.1f" % (name, noise(fp, 560, 640), mean(fp)))

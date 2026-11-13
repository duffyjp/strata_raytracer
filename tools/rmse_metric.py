import zlib, struct, os
def load(path):
    d = open(path, 'rb').read(); i = 8; w = h = 0; idat = b''
    while i < len(d):
        ln = struct.unpack('>I', d[i:i+4])[0]; typ = d[i+4:i+8]; data = d[i+8:i+8+ln]
        if typ == b'IHDR': w, h = struct.unpack('>II', data[:8])
        elif typ == b'IDAT': idat += data
        elif typ == b'IEND': break
        i += 12 + ln
    raw = zlib.decompress(idat); bpp = 3; px = bytearray(w*h*bpp); prev = bytearray(w*bpp); p = 0
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
        px[y*w*bpp:(y+1)*w*bpp] = line; prev = line
    return w, h, px
def first(d):
    return os.path.join(d, [x for x in sorted(os.listdir(d)) if x.endswith('.png')][0])
rw, rh, ref = load(first('cmp_ref'))
def rmse(path):
    w, h, px = load(path)
    if (w, h) != (rw, rh): return None
    tot = 0.0; n = 0
    for o in range(0, min(len(px), len(ref)) - 2, 3):
        dr = px[o]-ref[o]; dg = px[o+1]-ref[o+1]; db = px[o+2]-ref[o+2]
        tot += (dr*dr + dg*dg + db*db)/3.0; n += 1
    return (tot/n) ** 0.5
print("RMSE against the 512-frame converged reference (lower is closer to truth):")
for name in ['cmp_raw','cmp_a2','cmp_f2','cmp_a4','cmp_f4','cmp_prog','cmp_filt','cmp_ref']:
    if os.path.isdir(name):
        print("  %-10s %.2f" % (name, rmse(first(name))))

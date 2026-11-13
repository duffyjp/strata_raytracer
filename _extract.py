import os
OUT = r'C:\Users\Jacob\AppData\Local\Temp\rtp'

def extract(path, a, b, name, show=0):
    with open(path, 'r', encoding='utf-8', newline='') as f:
        lines = f.read().replace('\r\n', '\n').split('\n')
    body = lines[a-1:b]
    with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(body))
    print('%-22s %d lines | first=%r | last=%r' % (name, len(body), body[0], body[-1]))

extract('src/core.cpp', 357, 447, 'probe_old.txt')
extract('src/app.h', 47, 53, 'cpu_old.txt')
extract('src/app.h', 54, 54, 'appstruct_old.txt')
extract('src/app.h', 275, 275, 'faildecl_old.txt')
extract('src/app.h', 295, 295, 'pick_old.txt')
extract('src/main.cpp', 140, 144, 'assets_old.txt')

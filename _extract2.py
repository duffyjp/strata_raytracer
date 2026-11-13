import os
OUT = r'C:\Users\Jacob\AppData\Local\Temp\rtp'
p = 'src/core.cpp'
lines = open(p, encoding='utf-8', newline='').read().replace('\r\n','\n').split('\n')
start = next(i for i,l in enumerate(lines) if l.strip().startswith('// TEMPORARY SELF-TEST'))
end = next(i for i,l in enumerate(lines) if l.strip() == 'const AdapterCaps& cap = app->chosen;')
body = '\n'.join(lines[start:end+1])
open(os.path.join(OUT,'selftest2_old_now.txt'),'w',encoding='utf-8',newline='\n').write(body)
print('extracted lines %d..%d (%d lines)' % (start+1, end+1, end-start+1))
print('count in file:', open(p,encoding='utf-8',newline='').read().replace('\r\n','\n').count(body))

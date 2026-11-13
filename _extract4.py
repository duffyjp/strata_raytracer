import os
OUT = r'C:\Users\Jacob\AppData\Local\Temp\rtp'
p = 'src/core.cpp'
t = open(p, encoding='utf-8', newline='').read().replace('\r\n','\n')
lines = t.split('\n')
start = next(i for i,l in enumerate(lines) if l.strip().startswith('// TEMPORARY SELF-TEST'))
stop = next(i for i,l in enumerate(lines) if l.strip() == 'VkPhysicalDeviceVulkan12Features f12{};')
end = stop - 1
while lines[end].strip() == '':
    end -= 1
body = '\n'.join(lines[start:end+1])
assert t.count(body) == 1, t.count(body)
open(os.path.join(OUT,'selftest_any_now.txt'),'w',encoding='utf-8',newline='\n').write(body)
print('anchor lines %d..%d' % (start+1, end+1))

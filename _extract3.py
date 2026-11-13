import os
OUT = r'C:\Users\Jacob\AppData\Local\Temp\rtp'
p = 'src/core.cpp'
lines = open(p, encoding='utf-8', newline='').read().replace('\r\n','\n').split('\n')
start = next(i for i,l in enumerate(lines) if l.strip().startswith('// TEMPORARY SELF-TEST'))
end = next(i for i,l in enumerate(lines) if l.strip() == 'VkPhysicalDeviceVulkan12Features f12{};') - 1
body = '\n'.join(lines[start:end+1]).rstrip('\n')
open(os.path.join(OUT,'selftest_any_now.txt'),'w',encoding='utf-8',newline='\n').write(body)
print('block lines %d..%d, count=%d' % (start+1, end+1, open(p,encoding='utf-8',newline='').read().replace('\r\n','\n').count(body)))
print('--- first/last ---'); print(lines[start]); print(lines[end])

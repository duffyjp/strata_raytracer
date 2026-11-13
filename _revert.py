import os
OUT = r'C:\Users\Jacob\AppData\Local\Temp\rtp'
p = 'src/core.cpp'
t = open(p, encoding='utf-8', newline='').read().replace('\r\n','\n')

# 1. remove the RT_SELFTEST_FNP block (from its comment to the closing brace before f12)
lines = t.split('\n')
a = next(i for i,l in enumerate(lines) if l.strip().startswith('// TEMPORARY SELF-TEST'))
b = next(i for i,l in enumerate(lines) if l.strip() == 'VkPhysicalDeviceVulkan12Features f12{};') - 1
while lines[b].strip() == '}':
    b -= 1
assert lines[b+1].strip() == '}'
block = '\n'.join(lines[a:b+2])
assert t.count(block) == 1
t = t.replace(block + '\n\n', '', 1)

# 2. remove the RT_SIMULATE_NO_RAYQUERY lines
sim = '''
    // TEMPORARY SIMULATION - DELETE: hides ray queries so this machine can stand in for a GTX 1080 Ti
    // (ray-tracing pipeline yes, ray queries no).
    if (getenv("RT_SIMULATE_NO_RAYQUERY")) c->rqf.rayQuery = VK_FALSE;
'''
assert t.count(sim) == 1
t = t.replace(sim, '', 1)
open(p,'w',encoding='utf-8',newline='\n').write(t)
print('temp code removed; TEMPORARY left: %d' % t.count('TEMPORARY'))

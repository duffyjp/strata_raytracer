p='src/core.cpp'
t=open(p,encoding='utf-8',newline='').read().replace('\r\n','\n')
old='            }\n        }\n\n    VkPhysicalDeviceVulkan12Features f12{};'
new='            }\n        }\n    }\n\n    VkPhysicalDeviceVulkan12Features f12{};'
assert t.count(old)==1, t.count(old)
open(p,'w',encoding='utf-8',newline='\n').write(t.replace(old,new,1))
print('brace added')

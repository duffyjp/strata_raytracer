p='src/core.cpp'
t=open(p,encoding='utf-8',newline='').read().replace('\r\n','\n')
old='            {"variableSampleMask", &VkPhysicalDeviceFeatures::variableSampleMask},\n'
assert t.count(old)==1
t=t.replace(old,'            {"sparseResidencyImage3D", &VkPhysicalDeviceFeatures::sparseResidencyImage3D},\n',1)
open(p,'w',encoding='utf-8',newline='\n').write(t)
print('probe row replaced')

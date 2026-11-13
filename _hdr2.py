import re
t = open('third_party/vulkan_headers/vulkan/vulkan_core.h', encoding='utf-8', errors='replace').read()
m = re.search(r'typedef struct VkPhysicalDeviceFeatures \{(.*?)\} VkPhysicalDeviceFeatures;', t, re.S)
names = re.findall(r'VkBool32\s+(\w+);', m.group(1))
print(len(names), 'members')
print(', '.join(names))
for want in ['textureCompressionETC2','textureCompressionASTC_LDR','alphaToOne','variableSampleMask','shaderStorageImageReadWithoutFormat','geometryShader','tessellationShader']:
    print(want, want in names)

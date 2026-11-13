import re
p = 'third_party/vulkan_headers/vulkan/vulkan_core.h'
t = open(p, 'r', encoding='utf-8', errors='replace').read()
m = re.search(r'typedef struct VkPhysicalDeviceVulkan13Features \{(.*?)\} VkPhysicalDeviceVulkan13Features;', t, re.S)
print(m.group(1).strip())

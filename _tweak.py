p='src/core.cpp'
t=open(p,encoding='utf-8',newline='').read().replace('\r\n','\n')
old = '''        const bool useGeom = base.features.geometryShader == VK_FALSE;
        const bool haveTarget = useGeom || base.features.tessellationShader == VK_FALSE;
        const char* coreName = useGeom ? "geometryShader" : "tessellationShader";'''
new = '''        auto firstFalse = [&](void) -> const char* {
            const VkPhysicalDeviceFeatures& f = base.features;
            struct Row { const char* name; VkBool32 VkPhysicalDeviceFeatures::*m; };
            const Row rows[] = {
                {"textureCompressionETC2", &VkPhysicalDeviceFeatures::textureCompressionETC2},
                {"textureCompressionASTC_LDR", &VkPhysicalDeviceFeatures::textureCompressionASTC_LDR},
                {"alphaToOne", &VkPhysicalDeviceFeatures::alphaToOne},
                {"variableSampleMask", &VkPhysicalDeviceFeatures::variableSampleMask},
                {"shaderStorageImageReadWithoutFormat", &VkPhysicalDeviceFeatures::shaderStorageImageReadWithoutFormat},
                {"shaderStorageImageWriteWithoutFormat", &VkPhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat},
                {"geometryShader", &VkPhysicalDeviceFeatures::geometryShader},
                {"tessellationShader", &VkPhysicalDeviceFeatures::tessellationShader},
            };
            std::string seen;
            for (const Row& r : rows)
                if (!f.*(r.m)) { return r.name; }
            for (const Row& r : rows) { seen += r.name; seen += "="; seen += (f.*(r.m) ? "yes " : "no "); }
            logMsg("selftest b probe list: %s\\n", seen.c_str());
            return nullptr;
        };
        const char* coreName = firstFalse();
        const bool haveTarget = coreName != nullptr;
        bool useGeom = coreName && strcmp(coreName, "textureCompressionETC2") == 0;'''
assert t.count(old)==1
open(p,'w',encoding='utf-8',newline='\n').write(t.replace(old,new,1))
print('patched probe candidate list')

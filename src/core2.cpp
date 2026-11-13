// core2.cpp - pipelines, HDR targets, shader binding table, scene upload, frame recording.
//
// Split from core.cpp only to keep files readable; it is the same translation-unit-free design.
#include "app.h"
#include "generated/shaders_spv.h"

#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static VkShaderModule createShaderModule(App* app, const uint32_t* code, size_t words,
                                         const char* pakOverride) {
    static std::vector<uint8_t> blob;  // kept alive for the module's lifetime
    blob.clear();
    if (pakOverride && app->pakLoaded && app->pak.read(pakOverride, &blob) && blob.size() > 4) {
        code = reinterpret_cast<const uint32_t*>(blob.data());
        words = blob.size() / 4;
        if (app->verbose) logMsg("  shader override from pak: %s\n", pakOverride);
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * 4;
    ci.pCode = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(app->device, &ci, nullptr, &mod));
    return mod;
}

void createShaderModulesAndPipelines(App* app) {
    if (app->usingComputeFallback) {
        VkShaderModule mod = createShaderModule(app, shaders::kFallbackComp,
                                                shaders::kFallbackCompWords, "shaders/fallback.spv");
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = app->traceLayout;
        VK_CHECK(vkCreateComputePipelines(app->device, app->pipelineCache, 1, &ci, nullptr,
                                          &app->tracePipeline));
        vkDestroyShaderModule(app->device, mod, nullptr);
        createShaderBindingTable(app);
        return;
    }

    // Group order is the shader-binding-table record order:
    //   0 raygen | 1 miss(scene) | 2 miss(shadow) | 3 closest-hit(shade) | 4 closest-hit(shadow)
    VkShaderModule mods[5] = {};
    mods[0] = createShaderModule(app, shaders::kRaygen, shaders::kRaygenWords, "shaders/main.spv");
    mods[1] = createShaderModule(app, shaders::kMiss, shaders::kMissWords, "shaders/miss.spv");
    mods[2] = createShaderModule(app, shaders::kShadowMiss, shaders::kShadowMissWords,
                                 "shaders/shadow_miss.spv");
    mods[3] = createShaderModule(app, shaders::kClosestHit, shaders::kClosestHitWords, "shaders/hit.spv");
    mods[4] = createShaderModule(app, shaders::kShadowHit, shaders::kShadowHitWords,
                                 "shaders/shadow_hit.spv");

    const VkShaderStageFlagBits bits[5] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};

    VkPipelineShaderStageCreateInfo stages[5] = {};
    for (int i = 0; i < 5; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = bits[i];
        stages[i].module = mods[i];
        stages[i].pName = "main";
    }

    VkRayTracingShaderGroupCreateInfoKHR groups[5] = {};
    for (int i = 0; i < 5; ++i) {
        groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        // Vulkan-Headers 1.4 exposes these enumerators under both the KHR and the renamed
        // VkShaderGroupShaderKHR enum, so the conversion has to be explicit.
        groups[i].type = (i < 3)
                             ? static_cast<VkRayTracingShaderGroupTypeKHR>(VK_SHADER_GROUP_SHADER_GENERAL_KHR)
                             : static_cast<VkRayTracingShaderGroupTypeKHR>(VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR);
        groups[i].generalShader = (i < 3) ? uint32_t(i) : VK_SHADER_UNUSED_KHR;
        groups[i].closestHitShader = (i >= 3) ? uint32_t(i) : VK_SHADER_UNUSED_KHR;
        groups[i].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }

    VkRayTracingPipelineInterfaceCreateInfoKHR iface{};
    iface.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR;
    iface.maxPipelineRayPayloadSize = 16 * 4;  // struct RayHit: four vec4 = 16 dwords
    iface.maxPipelineRayHitAttributeSize = 3 * 4;

    VkRayTracingPipelineCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    ci.stageCount = 5;
    ci.pStages = stages;
    ci.groupCount = 5;
    ci.pGroups = groups;
    ci.maxPipelineRayRecursionDepth = 2;  // the transport loop is iterative: rgen -> chit only
    ci.pLibraryInterface = &iface;
    ci.layout = app->traceLayout;

    VK_CHECK(vkCreateRayTracingPipelinesKHR(app->device, VK_NULL_HANDLE, app->pipelineCache, 1, &ci,
                                            nullptr, &app->tracePipeline));
    for (int i = 0; i < 5; ++i) vkDestroyShaderModule(app->device, mods[i], nullptr);
    createShaderBindingTable(app);
}

// SBT: five stride-aligned records. Region bases sit on multiples of shaderGroupBaseAlignment, and
// each stride is a multiple of 16, satisfying vkCmdTraceRaysKHR's alignment requirements.
uint32_t sbtStrideFor(App* app) {
    uint32_t handle = std::max<uint32_t>(32, app->rtProps.shaderGroupHandleSize);
    uint32_t base = std::max<uint32_t>(16, app->rtProps.shaderGroupBaseAlignment);
    uint32_t stride = (handle + base - 1) / base * base;
    stride = (stride + 15) / 16 * 16;
    return stride;
}

void createShaderBindingTable(App* app) {
    if (app->usingComputeFallback) {
        createBuffer(app, 256, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true,
                     &app->sbt);
        memset(app->sbt.mapped, 0, size_t(app->sbt.size));
        return;
    }
    uint32_t stride = sbtStrideFor(app);
    uint32_t handle = app->rtProps.shaderGroupHandleSize;
    VkDeviceSize total = VkDeviceSize(stride) * 5;

    createBuffer(app, total, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true,
                 &app->sbt);
    std::vector<uint8_t> handles(size_t(handle) * 5, 0);
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(app->device, app->tracePipeline, 0, 5,
                                                  handles.size(), handles.data()));
    uint8_t* dst = static_cast<uint8_t*>(app->sbt.mapped);
    memset(dst, 0, size_t(total));
    for (int g = 0; g < 5; ++g)
        memcpy(dst + size_t(g) * stride, handles.data() + size_t(g) * handle, size_t(handle));
}

// -------------------------------------------------------------------------------------------------
void recreateHdrTargets(App* app, uint32_t width, uint32_t height) {
    width = std::max<uint32_t>(16, width);
    height = std::max<uint32_t>(16, height);
    app->hdrWidth = width;
    app->hdrHeight = height;

    const VkFormat wanted[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
                               VK_FORMAT_R16G16B16A16_UNORM};
    VkFormat chosen = VK_FORMAT_UNDEFINED;
    for (VkFormat f : wanted) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(app->physicalDevice, f, &fp);
        VkFormatFeatureFlags opt = fp.optimalTilingFeatures;
        if ((opt & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            (opt & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
            (opt & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) &&
            (opt & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
            chosen = f;
            break;
        }
    }
    if (chosen == VK_FORMAT_UNDEFINED) fail("no HDR image format supports storage+sampled+transfer");
    app->hdrFormat = chosen;

    for (int i = 0; i < kFramesInFlight; ++i) {
        destroyImage(app, &app->hdr[i]);
        createImage(app, width, height, chosen,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &app->hdr[i]);

        VkCommandBuffer cmd = beginOneTime(app);
        transitionImage(app, cmd, &app->hdr[i], VK_IMAGE_LAYOUT_GENERAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(cmd, app->hdr[i].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        endOneTime(app, cmd);
    }
    if (app->verbose)
        logMsg("  hdr target %ux%u, format %d\n", width, height, (int)chosen);
}

// -------------------------------------------------------------------------------------------------
void computeInstances(App* app, float t, std::vector<CpuInstance>* out) {
    out->clear();
    for (const auto& obj : app->cfg.objects) {
        if (out->size() >= size_t(kMaxInstances)) break;
        CpuInstance ci;
        ci.meshKind = (obj.kind == "cube")     ? kMeshCube
                      : (obj.kind == "cone")   ? kMeshCone
                      : (obj.kind == "disc" || obj.kind == "ground" || obj.kind == "plane")
                          ? kMeshDisc
                          : kMeshSphere;
        ci.material = app->cfg.materialIndexByName(obj.material);
        if (ci.material < 0) ci.material = 0;
        ci.isLight = obj.isLight;

        float pos[3] = {obj.position[0], obj.position[1], obj.position[2]};
        if (obj.isLight && obj.lightIndex >= 0 && obj.lightIndex < (int)app->cfg.lights.size()) {
            const LightDef& L = app->cfg.lights[obj.lightIndex];
            float ang = t * L.speed + L.phase;
            pos[0] = cosf(ang) * L.orbitRadius;
            pos[1] = L.orbitHeight;
            pos[2] = sinf(ang) * L.orbitRadius;
        }
        ci.xf = makeTransform(pos, obj.scale, obj.rotationY);
        out->push_back(ci);
    }
}

void fillSceneData(App* app, SceneGpuData* d, float t, uint32_t w, uint32_t h, bool reset,
                   int samplesForDisplay) {
    memset(d, 0, sizeof(*d));
    const CameraDef& C = app->cfg.camera;

    float ang = t * C.speed;
    float height = C.orbitHeight + C.heightBob * sinf(t * 0.23f);
    float o[3] = {cosf(ang) * C.orbitRadius, height, sinf(ang) * C.orbitRadius};

    float f[3] = {C.lookAt[0] - o[0], C.lookAt[1] - o[1], C.lookAt[2] - o[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) fl = 1.f;
    for (int i = 0; i < 3; ++i) f[i] /= fl;

    // right = normalize(cross(forward, worldUp));  up = cross(right, forward)
    // (cross(f, (0,1,0)) = (-f.z, 0, f.x)) - getting this sign wrong flips the whole image.
    float r[3] = {-f[2], 0.f, f[0]};
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-5f) { r[0] = 1.f; r[1] = 0.f; r[2] = 0.f; rl = 1.f; }
    for (int i = 0; i < 3; ++i) r[i] /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};

    float aspect = h ? float(w) / float(h) : 1.f;
    float tanHalf = tanf(C.fovDeg * float(M_PI) / 360.f);

    d->camera[0] = V4(o[0], o[1], o[2], tanHalf);
    d->camera[1] = V4(r[0], r[1], r[2], aspect);
    d->camera[2] = V4(u[0], u[1], u[2], 1.f);
    d->camera[3] = V4(f[0], f[1], f[2], float(app->cfg.maxBounces));
    d->camera[4] = V4(float(w), float(h), reset ? 1.f : 0.f, t);
    d->camera[5] = V4(app->cfg.exposure, float(app->frameIndex & 0x3FFFFu), app->cfg.radianceClamp,
                      float(app->cfg.skyMode));
    const int spp = app->cfg.autoQuality ? app->liveSpp : app->cfg.samplesPerFrame;
    // w = how many trace pixels sit behind one presented pixel; the present pass box-filters when it
    // is above 1, which is what turns supersampling into sharpness instead of shimmer.
    const float ssaa = app->swapExtent.width
                           ? float(w) / float(app->swapExtent.width)
                           : 1.f;
    // x = total samples behind the displayed image (the present shader divides by it),
    // y = flipY for the present pass, z = samples this pass adds per pixel.
    // x = frames accumulated into the target; the per-pixel sample count is divided out where the
    // samples are summed, so that the stored image means the same thing at any quality setting.
    d->camera[6] = V4(float(samplesForDisplay), 1.f, float(spp), ssaa);
    d->camera[7] = V4(app->cfg.skyColor[0], app->cfg.skyColor[1], app->cfg.skyColor[2], 0.f);

    for (size_t i = 0; i < app->cfg.materials.size() && i < size_t(kMaxMaterials); ++i) {
        const MaterialDef& m = app->cfg.materials[i];
        V4* p = &d->material[i * 4];
        p[0] = V4(m.albedo[0], m.albedo[1], m.albedo[2], m.diffuse);
        p[1] = V4(m.emissive[0], m.emissive[1], m.emissive[2], 0.f);
        p[2] = V4(m.reflectivity, m.roughness, m.ior, m.textureScale);
        p[3] = V4(float(m.model), float(m.textureSlot), 0.f, 0.f);
    }
    for (size_t i = 0; i < app->cfg.lights.size() && i < size_t(kMaxLights); ++i) {
        const LightDef& L = app->cfg.lights[i];
        float la = t * L.speed + L.phase;
        d->light[i * 2 + 0] = V4(cosf(la) * L.orbitRadius, L.orbitHeight, sinf(la) * L.orbitRadius,
                                 L.radius);
        d->light[i * 2 + 1] = V4(L.color[0] * L.intensity, L.color[1] * L.intensity,
                                 L.color[2] * L.intensity, L.on ? 1.f : 0.f);
    }
}

// -------------------------------------------------------------------------------------------------
void buildFrameCommand(App* app, int slotIdx, uint32_t imageIndex, bool includePresent,
                       const std::vector<CpuInstance>& instances, float t, bool reset,
                       int samplesForDisplay, bool traceIt) {
    VkCommandBuffer cmd = app->cmd[slotIdx];
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // The controller needs the GPU's own opinion of how long a frame took; the CPU cannot see it
    // while the loop is vsync-paced. Two timestamps around the buffer cost nothing and never stall.
    const uint32_t queryBase = uint32_t(slotIdx) * 2u;
    if (app->timeQueries) {
        vkCmdResetQueryPool(cmd, app->timeQueries, queryBase, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, app->timeQueries, queryBase);
    }

    // 1. Per-frame scene data.
    SceneGpuData* sceneData = static_cast<SceneGpuData*>(app->sceneUbo[slotIdx].mapped);
    fillSceneData(app, sceneData, t, app->hdrWidth, app->hdrHeight, reset, samplesForDisplay);

    const uint32_t instBytes = uint32_t(instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    const uint32_t xformBytes = uint32_t(instances.size() * sizeof(GpuInstance));

    VkBufferCopy c1{};  // VkBufferCopy has no sType (VkBufferCopy2 does)
    c1.size = instBytes;
    vkCmdCopyBuffer(cmd, app->instanceStage[slotIdx].buf, app->instanceDev[slotIdx].buf, 1, &c1);
    VkBufferCopy c2{};
    c2.size = xformBytes;
    vkCmdCopyBuffer(cmd, app->xformStage[slotIdx].buf, app->xformDev.buf, 1, &c2);

    // Uploaded data must be visible to the acceleration-structure build and to shaders.
    memBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
               VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                   VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT);

    // 2. Rebuild the TLAS: the light spheres move every frame.
    VkAccelerationStructureGeometryKHR geo{};
    geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo.geometry.instances.arrayOfPointers = VK_FALSE;
    geo.geometry.instances.data.deviceAddress = app->instanceDev[slotIdx].addr;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geo;
    buildInfo.dstAccelerationStructure = app->tlas[slotIdx];
    buildInfo.scratchData.deviceAddress = app->scratch[slotIdx].addr;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = uint32_t(instances.size());
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&range};
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);

    memBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
               VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
               VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    // 3. Accumulate one sample per pixel into the HDR target.
    VkPipelineStageFlags traceStage = app->usingComputeFallback
                                          ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                          : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    if (traceIt) {
        transitionImage(app, cmd, &app->hdr[slotIdx], VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, traceStage, VK_IMAGE_ASPECT_COLOR_BIT);

        // Reset means "start a new image": the ray shader skips reading the previous value, and the
        // accumulator is additionally zeroed explicitly from runCapture. Relying on the shader alone
        // left stale samples of earlier stills in the buffer (several exposures in one image).

        if (app->usingComputeFallback) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, app->traceLayout, 0, 1,
                                    &app->traceSet[slotIdx], 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, app->tracePipeline);
            vkCmdDispatch(cmd, (app->hdrWidth + 7) / 8, (app->hdrHeight + 7) / 8, 1);
        } else {
            uint32_t stride = sbtStrideFor(app);
            VkDeviceAddress base = app->sbt.addr;
            VkStridedDeviceAddressRegionKHR raygen{};
            raygen.deviceAddress = base + 0 * stride;
            raygen.stride = stride;
            raygen.size = stride;
            // Miss region holds records 1..2 (scene background, then shadow), hit region 3..4
            // (shade, then shadow) - matching the group order in the pipeline.
            VkStridedDeviceAddressRegionKHR miss{};
            miss.deviceAddress = base + 1 * stride;
            miss.stride = stride;
            miss.size = stride * 2;
            VkStridedDeviceAddressRegionKHR hit{};
            hit.deviceAddress = base + 3 * stride;
            hit.stride = stride;
            hit.size = stride * 2;
            VkStridedDeviceAddressRegionKHR callable{};  // this pipeline has no callable shaders
            callable.deviceAddress = 0;
            callable.stride = 0;
            callable.size = 0;

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, app->traceLayout, 0,
                                    1, &app->traceSet[slotIdx], 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, app->tracePipeline);
            vkCmdTraceRaysKHR(cmd, &raygen, &miss, &hit, &callable, app->hdrWidth, app->hdrHeight, 1);
        }
    }

    if (!includePresent) {
        if (app->timeQueries)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, app->timeQueries,
                                queryBase + 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        return;
    }

    // 4. Present: tone-map the accumulated radiance into the back buffer.
    memBarrier(cmd, traceStage, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT);
    transitionImage(app, cmd, &app->hdr[slotIdx], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, traceStage,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = app->renderPass;
    rp.framebuffer = app->framebuffers[imageIndex];
    rp.renderArea.extent = app->swapExtent;
    // The attachment loads with DONT_CARE, so no clear values are needed; being explicit here keeps
    // the count consistent if that ever changes.
    rp.clearValueCount = 0;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{};
    vp.x = 0;
    vp.y = 0;
    vp.width = float(app->swapExtent.width);
    vp.height = float(app->swapExtent.height);
    vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{};
    sc.extent = app->swapExtent;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app->presentPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app->presentLayout, 0, 1,
                            &app->presentSet[slotIdx], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    if (app->timeQueries)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, app->timeQueries,
                            queryBase + 1);
    VK_CHECK(vkEndCommandBuffer(cmd));
}

// -------------------------------------------------------------------------------------------------
void destroyApp(App* app) {
    if (app->device) vkDeviceWaitIdle(app->device);

    for (int i = 0; i < kFramesInFlight; ++i) {
        destroyBuffer(app, &app->sceneUbo[i]);
        destroyBuffer(app, &app->instanceStage[i]);
        destroyBuffer(app, &app->xformStage[i]);
        destroyBuffer(app, &app->instanceDev[i]);
        destroyBuffer(app, &app->scratch[i]);
        destroyImage(app, &app->hdr[i]);
        destroyImage(app, &app->hold[i]);
        app->holdPending[i] = false;
        if (app->tlas[i]) { vkDestroyAccelerationStructureKHR(app->device, app->tlas[i], nullptr); app->tlas[i] = VK_NULL_HANDLE; }
        destroyBuffer(app, &app->tlasBuffer[i]);
        if (app->fence[i]) { vkDestroyFence(app->device, app->fence[i], nullptr); app->fence[i] = VK_NULL_HANDLE; }
        if (app->imageReady[i]) { vkDestroySemaphore(app->device, app->imageReady[i], nullptr); app->imageReady[i] = VK_NULL_HANDLE; }
        if (app->drawDone[i]) { vkDestroySemaphore(app->device, app->drawDone[i], nullptr); app->drawDone[i] = VK_NULL_HANDLE; }
    }
    for (int k = 0; k < 4; ++k) {
        MeshGpu& m = app->meshes[k];
        if (m.blas) { vkDestroyAccelerationStructureKHR(app->device, m.blas, nullptr); m.blas = VK_NULL_HANDLE; }
        destroyBuffer(app, &m.asBuffer);
        destroyBuffer(app, &m.vb);
        destroyBuffer(app, &m.ib);
    }
    destroyBuffer(app, &app->xformDev);
    destroyBuffer(app, &app->sbt);
    destroyImage(app, &app->whiteTex);
    for (auto& im : app->textures) destroyImage(app, &im);
    app->textures.clear();

    if (app->sampler) { vkDestroySampler(app->device, app->sampler, nullptr); app->sampler = VK_NULL_HANDLE; }
    if (app->presentPipeline) vkDestroyPipeline(app->device, app->presentPipeline, nullptr);
    if (app->tracePipeline) vkDestroyPipeline(app->device, app->tracePipeline, nullptr);
    if (app->presentLayout) vkDestroyPipelineLayout(app->device, app->presentLayout, nullptr);
    if (app->traceLayout) vkDestroyPipelineLayout(app->device, app->traceLayout, nullptr);
    if (app->presentSetLayout) vkDestroyDescriptorSetLayout(app->device, app->presentSetLayout, nullptr);
    if (app->traceSetLayout) vkDestroyDescriptorSetLayout(app->device, app->traceSetLayout, nullptr);
    if (app->descPool) vkDestroyDescriptorPool(app->device, app->descPool, nullptr);
    if (app->pipelineCache) vkDestroyPipelineCache(app->device, app->pipelineCache, nullptr);

    vkDestroyRenderPass(app->device, app->renderPass, nullptr);
    for (VkFramebuffer fb : app->framebuffers) vkDestroyFramebuffer(app->device, fb, nullptr);
    for (VkImageView v : app->swapViews) vkDestroyImageView(app->device, v, nullptr);
    if (app->swapchain) vkDestroySwapchainKHR(app->device, app->swapchain, nullptr);
    if (app->cmdPool) vkDestroyCommandPool(app->device, app->cmdPool, nullptr);

    if (app->device) {
        if (!app->retiredCmd.empty()) {
            // Command buffers submitted without waiting during a quality change.
            vkFreeCommandBuffers(app->device, app->cmdPool, uint32_t(app->retiredCmd.size()),
                                 app->retiredCmd.data());
            app->retiredCmd.clear();
        }
        if (app->timeQueries) vkDestroyQueryPool(app->device, app->timeQueries, nullptr);
        app->timeQueries = VK_NULL_HANDLE;
        vkDestroyDevice(app->device, nullptr);
    }
    if (app->surface) vkDestroySurfaceKHR(app->instance, app->surface, nullptr);
    if (app->debugMessenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT fn =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(app->instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn) fn(app->instance, app->debugMessenger, nullptr);
    }
    if (app->instance) vkDestroyInstance(app->instance, nullptr);
}

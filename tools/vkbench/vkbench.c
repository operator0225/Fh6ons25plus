/* SPDX-License-Identifier: MIT
 *
 * vkbench -- Star Bionic Vulkan measurement harness.
 *
 * Answers three questions that gate Stages 4, 5 and 7, none of which need a
 * game running:
 *
 *   queues    Can this driver actually give us more than one queue? D3D12 has
 *             direct, compute and copy queues; if Vulkan exposes one,
 *             VKD3D-Proton must serialise all three onto it. The Turnip
 *             capture suggests exactly that -- this measures it.
 *
 *   pipeline  What does compiling a pipeline actually cost, cold vs. warm vs.
 *             seeded from a serialised cache? That is the Stage 5 baseline:
 *             without it, "the shader cache helped" is an opinion.
 *
 *   memory    Is the reported budget real? Allocate against it and watch it
 *             move. Turnip reports 2.80 GiB of an 8.14 GiB heap.
 *
 * Everything measured is reported; anything not measured is null, never a
 * plausible-looking zero. Same rule as vkprobe.
 *
 * Build (host):    cc -O2 -o vkbench vkbench.c -ldl
 * Build (Windows): ../vkprobe/build-windows.sh style; see build-windows.sh
 */

#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifdef _WIN32
  #include <windows.h>
  #define LIB_HANDLE   HMODULE
  #define LIB_OPEN(p)  LoadLibraryA(p)
  #define LIB_SYM(h,n) ((void *)(uintptr_t)GetProcAddress((h), (n)))
  #define LIB_ERR()    "LoadLibrary failed"
#else
  #include <dlfcn.h>
  #include <time.h>
  #define LIB_HANDLE   void *
  #define LIB_OPEN(p)  dlopen((p), RTLD_NOW | RTLD_LOCAL)
  #define LIB_SYM(h,n) dlsym((h), (n))
  #define LIB_ERR()    dlerror()
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bench_spv.h"

#define BENCH_VERSION "0.1.0"
#define DEFAULT_PIPELINES 48
#define DEFAULT_ALLOC_CHUNK_MB 128
#define DEFAULT_ALLOC_CAP_MB 10240
/*
 * Stop while there is still this much headroom left. On Android the failure
 * mode is NOT a clean VK_ERROR_OUT_OF_DEVICE_MEMORY -- the low-memory killer
 * takes the whole container first, so "allocate until the driver refuses"
 * never returns. Measured on an S25+: 7.00 GiB allocated fine at 0.53 GiB
 * headroom, then the container died. Stopping above that keeps the test
 * non-destructive while still producing the useful part of the curve.
 */
#define DEFAULT_HEADROOM_FLOOR_MB 1024

/* ------------------------------------------------------------------ */
/* timing                                                              */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ------------------------------------------------------------------ */
/* JSON (same minimal writer as vkprobe)                               */
/* ------------------------------------------------------------------ */

static int jdepth;
static int jneed[64];

static void jind(void) { for (int i = 0; i < jdepth; i++) fputs("  ", stdout); }

static void jsep(void)
{
    if (jneed[jdepth]) fputc(',', stdout);
    jneed[jdepth] = 1;
    fputc('\n', stdout);
    jind();
}

static void jesc(const char *s)
{
    fputc('"', stdout);
    if (s) for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout);  break;
        default:   if (*p < 0x20) printf("\\u%04x", *p); else fputc(*p, stdout);
        }
    }
    fputc('"', stdout);
}

static void jobj(const char *k)
{
    jsep();
    if (k) { jesc(k); fputs(": ", stdout); }
    fputc('{', stdout); jdepth++; jneed[jdepth] = 0;
}
static void jobj_end(void)
{
    int empty = !jneed[jdepth];
    jdepth--;
    if (!empty) { fputc('\n', stdout); jind(); }
    fputc('}', stdout);
}
static void jarr(const char *k)
{
    jsep();
    if (k) { jesc(k); fputs(": ", stdout); }
    fputc('[', stdout); jdepth++; jneed[jdepth] = 0;
}
static void jarr_end(void)
{
    int empty = !jneed[jdepth];
    jdepth--;
    if (!empty) { fputc('\n', stdout); jind(); }
    fputc(']', stdout);
}
static void jbool(const char *k, int v)
{ jsep(); jesc(k); fputs(": ", stdout); fputs(v ? "true" : "false", stdout); }
static void ju32(const char *k, uint32_t v) { jsep(); jesc(k); printf(": %u", v); }
static void ju64(const char *k, uint64_t v)
{ jsep(); jesc(k); printf(": %llu", (unsigned long long)v); }
static void jf(const char *k, double v) { jsep(); jesc(k); printf(": %.3f", v); }
static void jstr(const char *k, const char *v)
{ jsep(); jesc(k); fputs(": ", stdout); jesc(v); }
static void jnull(const char *k) { jsep(); jesc(k); fputs(": null", stdout); }

/* ------------------------------------------------------------------ */
/* GPU state from KGSL sysfs                                           */
/* ------------------------------------------------------------------ */

/*
 * Everything measured under sustained load varies run to run -- the 65536
 * level's worst frame moved 14% between two runs while its best frame moved
 * 0.06%. That is DVFS, and it was invisible because clock and temperature
 * were never sampled alongside the timings.
 *
 * Wine maps the Unix root at Z:, so a Windows binary inside the container can
 * read Android's sysfs directly. Sampling it here rather than from a separate
 * Termux process means the readings are in the same process, at the same
 * instant, as the frame they explain.
 */
static int read_sys_long(const char *rel, long *out)
{
    char path[512];
#ifdef _WIN32
    snprintf(path, sizeof path, "Z:\\%s", rel);
    for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
#else
    snprintf(path, sizeof path, "/%s", rel);
#endif
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    long v = 0;
    int ok = fscanf(fp, "%ld", &v) == 1;
    fclose(fp);
    if (ok) *out = v;
    return ok;
}

#define KGSL "sys/class/kgsl/kgsl-3d0/"

static int gpu_state_available(void)
{
    long v;
    return read_sys_long(KGSL "gpuclk", &v) ||
           read_sys_long(KGSL "devfreq/cur_freq", &v) ||
           read_sys_long(KGSL "temp", &v);
}

/* Emits a {gpu_mhz, gpu_temp_c, throttling, busy_pct} object under `key`.
 * Anything unreadable is null, never a plausible zero. */
static void emit_gpu_state(const char *key)
{
    long clk = 0, temp = 0, thr = 0, busy = 0;
    int have_clk = read_sys_long(KGSL "gpuclk", &clk) ||
                   read_sys_long(KGSL "devfreq/cur_freq", &clk);
    int have_temp = read_sys_long(KGSL "temp", &temp);
    int have_thr  = read_sys_long(KGSL "throttling", &thr);
    int have_busy = read_sys_long(KGSL "gpu_busy_percentage", &busy);

    jobj(key);
    if (have_clk) {
        /* gpuclk and devfreq/cur_freq report Hz; some kernels report MHz. */
        long mhz = clk > 10000000 ? clk / 1000000 : (clk > 10000 ? clk / 1000 : clk);
        ju32("gpu_mhz", (uint32_t)mhz);
    } else jnull("gpu_mhz");
    if (have_temp) jf("gpu_temp_c", temp > 1000 ? temp / 1000.0 : (double)temp);
    else jnull("gpu_temp_c");
    if (have_thr)  ju32("throttling", (uint32_t)thr); else jnull("throttling");
    if (have_busy) ju32("gpu_busy_pct", (uint32_t)busy); else jnull("gpu_busy_pct");
    jobj_end();
}

/* ------------------------------------------------------------------ */
/* loader                                                              */
/* ------------------------------------------------------------------ */

static PFN_vkGetInstanceProcAddr p_GIPA;
static PFN_vkGetDeviceProcAddr   p_GDPA;

#define IFN(name) ((PFN_vk##name)(uintptr_t)p_GIPA(inst, "vk" #name))
#define DFN(name) ((PFN_vk##name)(uintptr_t)p_GDPA(dev, "vk" #name))

static const char *g_libpath;

static int load_loader(const char *path)
{
    static const char *cands[] = {
#ifdef _WIN32
        "vulkan-1.dll",
#else
        "/system/lib64/libvulkan.so", "/system/lib/libvulkan.so",
        "libvulkan.so.1", "libvulkan.so",
#endif
        NULL,
    };
    LIB_HANDLE lib = NULL;
    if (path) { lib = LIB_OPEN(path); g_libpath = path; }
    else for (int i = 0; cands[i]; i++)
        if ((lib = LIB_OPEN(cands[i]))) { g_libpath = cands[i]; break; }

    if (!lib) { fprintf(stderr, "vkbench: cannot load loader: %s\n", LIB_ERR()); return 0; }
    p_GIPA = (PFN_vkGetInstanceProcAddr)(uintptr_t)LIB_SYM(lib, "vkGetInstanceProcAddr");
    return p_GIPA != NULL;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* shared state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties props;
    VkQueueFamilyProperties *qf;
    uint32_t qf_count;
    VkExtensionProperties *ext;
    uint32_t ext_count;
    int have_feedback;
    int have_budget;
} Ctx;

/* ------------------------------------------------------------------ */
/* GPU work harness -- shared by the queue-cost and frame-loop tests   */
/* ------------------------------------------------------------------ */

typedef struct {
    PFN_vkGetDeviceQueue        GetDeviceQueue;
    PFN_vkCreateCommandPool     CreateCommandPool;
    PFN_vkDestroyCommandPool    DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer    BeginCommandBuffer;
    PFN_vkEndCommandBuffer      EndCommandBuffer;
    PFN_vkQueueSubmit           QueueSubmit;
    PFN_vkQueueWaitIdle         QueueWaitIdle;
    PFN_vkDeviceWaitIdle        DeviceWaitIdle;
    PFN_vkCreateQueryPool       CreateQueryPool;
    PFN_vkDestroyQueryPool      DestroyQueryPool;
    PFN_vkCmdResetQueryPool     CmdResetQueryPool;
    PFN_vkCmdWriteTimestamp     CmdWriteTimestamp;
    PFN_vkGetQueryPoolResults   GetQueryPoolResults;
    PFN_vkCreateBuffer          CreateBuffer;
    PFN_vkDestroyBuffer         DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory      BindBufferMemory;
    PFN_vkAllocateMemory        AllocateMemory;
    PFN_vkFreeMemory            FreeMemory;
    PFN_vkCmdCopyBuffer         CmdCopyBuffer;
    PFN_vkCmdDispatch           CmdDispatch;
    PFN_vkCmdBindPipeline       CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPipelineBarrier    CmdPipelineBarrier;
    PFN_vkCreateDescriptorPool  CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets  UpdateDescriptorSets;
    PFN_vkCreateShaderModule    CreateShaderModule;
    PFN_vkDestroyShaderModule   DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout   CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout  DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout  CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline       DestroyPipeline;
} DevFn;

static int load_devfn(VkDevice dev, DevFn *f)
{
#define L(n) f->n = (PFN_vk##n)(uintptr_t)p_GDPA(dev, "vk" #n); if (!f->n) return 0
    L(GetDeviceQueue); L(CreateCommandPool); L(DestroyCommandPool);
    L(AllocateCommandBuffers); L(BeginCommandBuffer); L(EndCommandBuffer);
    L(QueueSubmit); L(QueueWaitIdle); L(DeviceWaitIdle);
    L(CreateQueryPool); L(DestroyQueryPool); L(CmdResetQueryPool);
    L(CmdWriteTimestamp); L(GetQueryPoolResults);
    L(CreateBuffer); L(DestroyBuffer); L(GetBufferMemoryRequirements);
    L(BindBufferMemory); L(AllocateMemory); L(FreeMemory);
    L(CmdCopyBuffer); L(CmdDispatch); L(CmdBindPipeline);
    L(CmdBindDescriptorSets); L(CmdPipelineBarrier);
    L(CreateDescriptorPool); L(DestroyDescriptorPool);
    L(AllocateDescriptorSets); L(UpdateDescriptorSets);
    L(CreateShaderModule); L(DestroyShaderModule);
    L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout);
    L(CreatePipelineLayout); L(DestroyPipelineLayout);
    L(CreateComputePipelines); L(DestroyPipeline);
#undef L
    return 1;
}

/* A compute pipeline plus the buffers the workload runs against. */
typedef struct {
    VkShaderModule mod;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkBuffer work, src, dst;
    VkDeviceMemory work_mem, src_mem, dst_mem;
    VkDeviceSize copy_bytes;
    int ok;
} Workload;

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties *mp,
                              uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (mp->memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int make_buffer(const DevFn *f, VkDevice dev,
                       const VkPhysicalDeviceMemoryProperties *mp,
                       VkDeviceSize size, VkBufferUsageFlags usage,
                       VkBuffer *buf, VkDeviceMemory *mem)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (f->CreateBuffer(dev, &bci, NULL, buf) != VK_SUCCESS) return 0;

    VkMemoryRequirements req;
    f->GetBufferMemoryRequirements(dev, *buf, &req);
    uint32_t type = find_mem_type(mp, req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        type = find_mem_type(mp, req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == UINT32_MAX) { f->DestroyBuffer(dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }

    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type,
    };
    if (f->AllocateMemory(dev, &ai, NULL, mem) != VK_SUCCESS) {
        f->DestroyBuffer(dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0;
    }
    f->BindBufferMemory(dev, *buf, *mem, 0);
    return 1;
}

static void workload_destroy(const DevFn *f, VkDevice dev, Workload *w)
{
    if (w->pipe)    f->DestroyPipeline(dev, w->pipe, NULL);
    if (w->pool)    f->DestroyDescriptorPool(dev, w->pool, NULL);
    if (w->layout)  f->DestroyPipelineLayout(dev, w->layout, NULL);
    if (w->dsl)     f->DestroyDescriptorSetLayout(dev, w->dsl, NULL);
    if (w->mod)     f->DestroyShaderModule(dev, w->mod, NULL);
    if (w->work)    f->DestroyBuffer(dev, w->work, NULL);
    if (w->src)     f->DestroyBuffer(dev, w->src, NULL);
    if (w->dst)     f->DestroyBuffer(dev, w->dst, NULL);
    if (w->work_mem) f->FreeMemory(dev, w->work_mem, NULL);
    if (w->src_mem)  f->FreeMemory(dev, w->src_mem, NULL);
    if (w->dst_mem)  f->FreeMemory(dev, w->dst_mem, NULL);
    memset(w, 0, sizeof *w);
}

static int workload_create(const DevFn *f, VkDevice dev,
                           const VkPhysicalDeviceMemoryProperties *mp,
                           int32_t variant, VkDeviceSize work_bytes,
                           VkDeviceSize copy_bytes, Workload *w)
{
    memset(w, 0, sizeof *w);
    w->copy_bytes = copy_bytes;

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof bench_comp_spv, .pCode = bench_comp_spv,
    };
    if (f->CreateShaderModule(dev, &smci, NULL, &w->mod) != VK_SUCCESS) goto fail;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind,
    };
    if (f->CreateDescriptorSetLayout(dev, &dslci, NULL, &w->dsl) != VK_SUCCESS) goto fail;

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &w->dsl,
    };
    if (f->CreatePipelineLayout(dev, &plci, NULL, &w->layout) != VK_SUCCESS) goto fail;

    VkSpecializationMapEntry entry = { 0, 0, sizeof variant };
    VkSpecializationInfo spec = {
        .mapEntryCount = 1, .pMapEntries = &entry,
        .dataSize = sizeof variant, .pData = &variant,
    };
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = w->mod,
            .pName = "main", .pSpecializationInfo = &spec,
        },
        .layout = w->layout,
    };
    if (f->CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &w->pipe) != VK_SUCCESS)
        goto fail;

    if (!make_buffer(f, dev, mp, work_bytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &w->work, &w->work_mem)) goto fail;
    if (!make_buffer(f, dev, mp, copy_bytes,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &w->src, &w->src_mem)) goto fail;
    if (!make_buffer(f, dev, mp, copy_bytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, &w->dst, &w->dst_mem)) goto fail;

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
    };
    if (f->CreateDescriptorPool(dev, &dpci, NULL, &w->pool) != VK_SUCCESS) goto fail;

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = w->pool, .descriptorSetCount = 1, .pSetLayouts = &w->dsl,
    };
    if (f->AllocateDescriptorSets(dev, &dsai, &w->set) != VK_SUCCESS) goto fail;

    VkDescriptorBufferInfo dbi = { w->work, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = w->set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi,
    };
    f->UpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    w->ok = 1;
    return 1;
fail:
    workload_destroy(f, dev, w);
    return 0;
}

/* Records `dispatches` compute dispatches and `copies` buffer copies into one
 * command buffer, in the order a streaming renderer would issue them. */
static void record(const DevFn *f, VkCommandBuffer cb, const Workload *w,
                   uint32_t groups, uint32_t dispatches, uint32_t copies,
                   int interleave)
{
    VkMemoryBarrier bar = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
    };
    VkBufferCopy region = { 0, 0, w->copy_bytes };
    uint32_t n = dispatches > copies ? dispatches : copies;

    if (dispatches) {
        f->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, w->pipe);
        f->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, w->layout,
                                 0, 1, &w->set, 0, NULL);
    }
    for (uint32_t i = 0; i < n; i++) {
        if (i < dispatches) {
            f->CmdDispatch(cb, groups, 1, 1);
            if (interleave)
                f->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                      1, &bar, 0, NULL, 0, NULL);
        }
        if (i < copies) {
            f->CmdCopyBuffer(cb, w->src, w->dst, 1, &region);
            if (interleave)
                f->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                      1, &bar, 0, NULL, 0, NULL);
        }
    }
}

/*
 * Submits one command buffer and returns wall time, plus GPU time from
 * timestamp queries when the queue supports them. Returns 0 on failure so a
 * caller can report "not measured" instead of a fabricated number.
 */
static int timed_submit(const DevFn *f, VkDevice dev, VkQueue q,
                        VkCommandBuffer cb, VkQueryPool qp, float ts_period,
                        uint32_t ts_valid_bits,
                        double *wall_ms, double *gpu_ms)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    (void)bi;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };

    double t0 = now_ms();
    if (f->QueueSubmit(q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return 0;
    if (f->QueueWaitIdle(q) != VK_SUCCESS) return 0;
    *wall_ms = now_ms() - t0;

    *gpu_ms = -1.0;
    if (qp && ts_valid_bits) {
        uint64_t ts[2] = { 0, 0 };
        if (f->GetQueryPoolResults(dev, qp, 0, 2, sizeof ts, ts, sizeof ts[0],
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
            == VK_SUCCESS) {
            uint64_t mask = ts_valid_bits >= 64 ? ~0ull : ((1ull << ts_valid_bits) - 1);
            uint64_t d = (ts[1] & mask) - (ts[0] & mask);
            *gpu_ms = (double)d * (double)ts_period / 1e6;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* TEST 1 -- queue topology                                            */
/* ------------------------------------------------------------------ */

/*
 * D3D12 wants three independent queues. Reporting the family list is not
 * enough: what matters is how many queues can actually be created and used
 * at once, which is bounded by queueCount per family. A single universal
 * queue means every D3D12 queue type serialises onto it.
 */
static void test_queues(Ctx *c)
{
    jobj("queues");

    int gfx_family = -1, dedicated_compute = -1, dedicated_transfer = -1;
    uint32_t total_queues = 0, universal_queues = 0;

    jarr("families");
    for (uint32_t i = 0; i < c->qf_count; i++) {
        VkQueueFlags f = c->qf[i].queueFlags;
        int g = (f & VK_QUEUE_GRAPHICS_BIT) != 0;
        int cm = (f & VK_QUEUE_COMPUTE_BIT) != 0;
        int t = (f & VK_QUEUE_TRANSFER_BIT) != 0;

        total_queues += c->qf[i].queueCount;
        if (g && cm) universal_queues += c->qf[i].queueCount;
        if (g && gfx_family < 0) gfx_family = (int)i;
        if (cm && !g && dedicated_compute < 0) dedicated_compute = (int)i;
        if (t && !g && !cm && dedicated_transfer < 0) dedicated_transfer = (int)i;

        jobj(NULL);
        ju32("index", i);
        ju32("queueCount", c->qf[i].queueCount);
        ju32("timestampValidBits", c->qf[i].timestampValidBits);
        jbool("graphics", g);
        jbool("compute", cm);
        jbool("transfer", t);
        jbool("sparse_binding", (f & VK_QUEUE_SPARSE_BINDING_BIT) != 0);
        jobj_end();
    }
    jarr_end();

    ju32("family_count", c->qf_count);
    ju32("total_queues", total_queues);
    ju32("universal_queues", universal_queues);

    if (gfx_family >= 0) ju32("graphics_family", (uint32_t)gfx_family);
    else jnull("graphics_family");
    if (dedicated_compute >= 0) ju32("dedicated_compute_family", (uint32_t)dedicated_compute);
    else jnull("dedicated_compute_family");
    if (dedicated_transfer >= 0) ju32("dedicated_transfer_family", (uint32_t)dedicated_transfer);
    else jnull("dedicated_transfer_family");

    /* The D3D12 verdict, stated in terms of what was actually found. */
    uint32_t gfx_count = gfx_family >= 0 ? c->qf[gfx_family].queueCount : 0;
    int can_parallel = (dedicated_compute >= 0) || (dedicated_transfer >= 0) || gfx_count > 1;

    jobj("d3d12_mapping");
    ju32("graphics_family_queue_count", gfx_count);
    jbool("separate_queues_possible", can_parallel);
    jstr("consequence", can_parallel
         ? "D3D12 direct/compute/copy queues can map to distinct Vulkan queues"
         : "only one usable queue: D3D12 direct, compute and copy all serialise "
           "onto it, so uploads cannot overlap rendering");
    jobj_end();

    jobj_end();
}

/* ------------------------------------------------------------------ */
/* TEST 2 -- pipeline creation cost                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    double wall_ms;
    uint64_t feedback_total_ns;
    uint32_t feedback_valid;
    uint32_t cache_hits;
} PipeRun;

/*
 * Creates `n` compute pipelines that differ by specialization constant, so
 * each is a genuinely distinct compile rather than a deduplicated repeat.
 */
static int run_pipelines(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa,
                         VkPipelineCache cache, VkShaderModule mod,
                         VkPipelineLayout layout, uint32_t n,
                         int have_feedback, PipeRun *out)
{
    PFN_vkCreateComputePipelines pCreate =
        (PFN_vkCreateComputePipelines)(uintptr_t)gdpa(dev, "vkCreateComputePipelines");
    PFN_vkDestroyPipeline pDestroy =
        (PFN_vkDestroyPipeline)(uintptr_t)gdpa(dev, "vkDestroyPipeline");
    if (!pCreate || !pDestroy) return 0;

    VkPipeline *pipes = calloc(n, sizeof *pipes);
    if (!pipes) return 0;

    memset(out, 0, sizeof *out);
    double t0 = now_ms();

    for (uint32_t i = 0; i < n; i++) {
        int32_t variant = (int32_t)(i + 1);
        VkSpecializationMapEntry entry = { 0, 0, sizeof variant };
        VkSpecializationInfo spec = {
            .mapEntryCount = 1, .pMapEntries = &entry,
            .dataSize = sizeof variant, .pData = &variant,
        };

        VkPipelineCreationFeedback fb = { 0 };
        VkPipelineCreationFeedback stage_fb = { 0 };
        VkPipelineCreationFeedbackCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
            .pPipelineCreationFeedback = &fb,
            .pipelineStageCreationFeedbackCount = 1,
            .pPipelineStageCreationFeedbacks = &stage_fb,
        };

        VkComputePipelineCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = have_feedback ? &fbci : NULL,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = mod,
                .pName = "main",
                .pSpecializationInfo = &spec,
            },
            .layout = layout,
        };

        if (pCreate(dev, cache, 1, &ci, NULL, &pipes[i]) != VK_SUCCESS) {
            pipes[i] = VK_NULL_HANDLE;
            continue;
        }
        if (have_feedback &&
            (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {
            out->feedback_valid++;
            out->feedback_total_ns += fb.duration;
            if (fb.flags &
                VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)
                out->cache_hits++;
        }
    }

    out->wall_ms = now_ms() - t0;

    for (uint32_t i = 0; i < n; i++)
        if (pipes[i]) pDestroy(dev, pipes[i], NULL);
    free(pipes);
    return 1;
}

static void emit_run(const char *name, const PipeRun *r, uint32_t n, int have_feedback)
{
    jobj(name);
    jf("wall_ms", r->wall_ms);
    jf("wall_ms_per_pipeline", n ? r->wall_ms / n : 0.0);
    if (have_feedback && r->feedback_valid) {
        ju32("feedback_valid_count", r->feedback_valid);
        jf("driver_reported_ms", r->feedback_total_ns / 1e6);
        ju32("cache_hits", r->cache_hits);
    } else {
        jnull("feedback_valid_count");
        jnull("driver_reported_ms");
        jnull("cache_hits");
    }
    jobj_end();
}

static void test_pipelines(Ctx *c, VkDevice dev, uint32_t n)
{
    VkPhysicalDevice pd = c->pd; (void)pd;
    PFN_vkGetDeviceProcAddr gdpa = p_GDPA;

    jobj("pipeline_creation");
    ju32("pipeline_count", n);
    jbool("pipeline_creation_feedback", c->have_feedback);

    PFN_vkCreateShaderModule pCSM = DFN(CreateShaderModule);
    PFN_vkDestroyShaderModule pDSM = DFN(DestroyShaderModule);
    PFN_vkCreateDescriptorSetLayout pCDSL = DFN(CreateDescriptorSetLayout);
    PFN_vkDestroyDescriptorSetLayout pDDSL = DFN(DestroyDescriptorSetLayout);
    PFN_vkCreatePipelineLayout pCPL = DFN(CreatePipelineLayout);
    PFN_vkDestroyPipelineLayout pDPL = DFN(DestroyPipelineLayout);
    PFN_vkCreatePipelineCache pCPC = DFN(CreatePipelineCache);
    PFN_vkDestroyPipelineCache pDPC = DFN(DestroyPipelineCache);
    PFN_vkGetPipelineCacheData pGPCD = DFN(GetPipelineCacheData);

    if (!pCSM || !pCDSL || !pCPL || !pCPC) {
        jstr("error", "required device entry points unavailable");
        jobj_end();
        return;
    }

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof bench_comp_spv,
        .pCode = bench_comp_spv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    if (pCSM(dev, &smci, NULL, &mod) != VK_SUCCESS) {
        jstr("error", "vkCreateShaderModule failed");
        jobj_end();
        return;
    }

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    pCDSL(dev, &dslci, NULL, &dsl);

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    pCPL(dev, &plci, NULL, &layout);

    VkPipelineCacheCreateInfo pcci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
    };

    /* --- cold: empty cache, nothing to hit --- */
    VkPipelineCache cold = VK_NULL_HANDLE;
    pCPC(dev, &pcci, NULL, &cold);
    PipeRun r_cold;
    run_pipelines(dev, gdpa, cold, mod, layout, n, c->have_feedback, &r_cold);
    emit_run("cold", &r_cold, n, c->have_feedback);

    /* --- warm: same cache object, identical pipelines --- */
    PipeRun r_warm;
    run_pipelines(dev, gdpa, cold, mod, layout, n, c->have_feedback, &r_warm);
    emit_run("warm_same_cache", &r_warm, n, c->have_feedback);

    /* --- seeded: a NEW cache built from the serialised blob. This is the
     * case Stage 5 actually cares about -- a cache reloaded from disk on a
     * later run, not one that happened to still be in memory. --- */
    size_t blob_size = 0;
    void *blob = NULL;
    if (pGPCD && pGPCD(dev, cold, &blob_size, NULL) == VK_SUCCESS && blob_size) {
        blob = malloc(blob_size);
        if (blob && pGPCD(dev, cold, &blob_size, blob) != VK_SUCCESS) {
            free(blob); blob = NULL;
        }
    }
    ju64("cache_blob_bytes", (uint64_t)blob_size);

    if (blob) {
        VkPipelineCacheCreateInfo seeded_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .initialDataSize = blob_size,
            .pInitialData = blob,
        };
        VkPipelineCache seeded = VK_NULL_HANDLE;
        if (pCPC(dev, &seeded_ci, NULL, &seeded) == VK_SUCCESS) {
            PipeRun r_seed;
            run_pipelines(dev, gdpa, seeded, mod, layout, n, c->have_feedback, &r_seed);
            emit_run("seeded_from_blob", &r_seed, n, c->have_feedback);
            if (pDPC) pDPC(dev, seeded, NULL);
        }
        free(blob);
    } else {
        jnull("seeded_from_blob");
    }

    /* Headline: what a persistent cache is actually worth here. */
    if (r_cold.wall_ms > 0)
        jf("warm_speedup_x", r_cold.wall_ms / (r_warm.wall_ms > 0 ? r_warm.wall_ms : 1e-9));
    else
        jnull("warm_speedup_x");

    if (pDPC) pDPC(dev, cold, NULL);
    if (pDPL) pDPL(dev, layout, NULL);
    if (pDDSL) pDDSL(dev, dsl, NULL);
    if (pDSM) pDSM(dev, mod, NULL);

    jobj_end();
}

/* ------------------------------------------------------------------ */
/* TEST 3 -- memory budget under pressure                              */
/* ------------------------------------------------------------------ */

static void query_budget(Ctx *c, PFN_vkGetPhysicalDeviceMemoryProperties2 pGMP2,
                         uint32_t heap, uint64_t *budget, uint64_t *usage)
{
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
    };
    VkPhysicalDeviceMemoryProperties2 mp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        .pNext = c->have_budget ? &b : NULL,
    };
    pGMP2(c->pd, &mp);
    *budget = c->have_budget ? b.heapBudget[heap] : 0;
    *usage  = c->have_budget ? b.heapUsage[heap] : 0;
}

/*
 * Allocates in chunks until the driver refuses or the cap is reached, then
 * frees everything. The point is not "how much can we take" -- it is whether
 * the advertised budget corresponds to what is actually obtainable.
 */
static void test_memory(Ctx *c, VkDevice dev, uint32_t chunk_mb, uint32_t cap_mb,
                        uint32_t floor_mb)
{
    PFN_vkGetPhysicalDeviceMemoryProperties2 pGMP2 =
        (PFN_vkGetPhysicalDeviceMemoryProperties2)(uintptr_t)
        p_GIPA(c->inst, "vkGetPhysicalDeviceMemoryProperties2");
    PFN_vkAllocateMemory pAlloc = DFN(AllocateMemory);
    PFN_vkFreeMemory pFree = DFN(FreeMemory);
    PFN_vkGetPhysicalDeviceMemoryProperties pGMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)(uintptr_t)
        p_GIPA(c->inst, "vkGetPhysicalDeviceMemoryProperties");

    jobj("memory");
    jbool("memory_budget_extension", c->have_budget);
    ju32("headroom_floor_mb", floor_mb);

    if (!pGMP2 || !pAlloc || !pFree || !pGMP) {
        jstr("error", "required entry points unavailable");
        jobj_end();
        return;
    }

    VkPhysicalDeviceMemoryProperties base;
    memset(&base, 0, sizeof base);
    pGMP(c->pd, &base);

    /* Largest DEVICE_LOCAL heap, and a memory type that lives on it. */
    uint32_t heap = 0, type = UINT32_MAX;
    VkDeviceSize best = 0;
    for (uint32_t i = 0; i < base.memoryHeapCount; i++)
        if ((base.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            base.memoryHeaps[i].size > best) { best = base.memoryHeaps[i].size; heap = i; }
    for (uint32_t i = 0; i < base.memoryTypeCount; i++)
        if (base.memoryTypes[i].heapIndex == heap &&
            (base.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i; break;
        }

    ju32("heap_index", heap);
    ju64("heap_size_bytes", (uint64_t)best);
    if (type == UINT32_MAX) {
        jstr("error", "no device-local memory type on the largest heap");
        jobj_end();
        return;
    }
    ju32("memory_type", type);

    uint64_t b0 = 0, u0 = 0;
    query_budget(c, pGMP2, heap, &b0, &u0);
    ju64("initial_budget_bytes", b0);
    ju64("initial_usage_bytes", u0);
    jf("initial_budget_gib", b0 / (double)(1ull << 30));

    const VkDeviceSize chunk = (VkDeviceSize)chunk_mb * 1024 * 1024;
    uint32_t max_chunks = cap_mb / chunk_mb;
    VkDeviceMemory *mem = calloc(max_chunks ? max_chunks : 1, sizeof *mem);
    uint32_t got = 0;
    VkResult last = VK_SUCCESS;
    int stopped_on_floor = 0;
    const uint64_t floor_bytes = (uint64_t)floor_mb * 1024ull * 1024ull;

    jarr("steps");
    for (uint32_t i = 0; i < max_chunks; i++) {
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = chunk,
            .memoryTypeIndex = type,
        };
        last = pAlloc(dev, &ai, NULL, &mem[i]);
        if (last != VK_SUCCESS) { mem[i] = VK_NULL_HANDLE; break; }
        got++;

        /* Budget is checked EVERY step, not just on reported ones: the floor
         * is a safety valve and sampling it every 8th chunk could step past
         * the cliff between checks. */
        uint64_t b = 0, u = 0;
        query_budget(c, pGMP2, heap, &b, &u);
        uint64_t headroom = b > u ? b - u : 0;

        /* Report every 8th step; the full curve is noise, the shape is not. */
        if ((got % 8) == 0 || got == 1 ||
            (c->have_budget && headroom < floor_bytes)) {
            jobj(NULL);
            ju32("allocated_mb", got * chunk_mb);
            ju64("budget_bytes", b);
            ju64("usage_bytes", u);
            /* budget is usage + estimated remaining, so it RISES as you
             * allocate. Headroom is the figure that actually falls, and the
             * one that predicts where allocation stops. */
            ju64("headroom_bytes", headroom);
            jobj_end();
            /* A killed container loses everything still buffered, so commit
             * each reported step to the file as it happens. */
            fflush(stdout);
        }

        if (c->have_budget && headroom < floor_bytes) {
            stopped_on_floor = 1;
            break;
        }
    }
    jarr_end();
    jbool("stopped_on_headroom_floor", stopped_on_floor);

    ju32("allocated_mb_total", got * chunk_mb);
    jf("allocated_gib_total", got * (double)chunk_mb / 1024.0);
    jbool("hit_allocation_failure", last != VK_SUCCESS);
    if (last != VK_SUCCESS) ju32("failure_result", (uint32_t)(-last));
    jbool("hit_cap", got == max_chunks);

    uint64_t b1 = 0, u1 = 0;
    query_budget(c, pGMP2, heap, &b1, &u1);
    ju64("budget_at_peak_bytes", b1);
    ju64("usage_at_peak_bytes", u1);

    /* Always give it back. */
    for (uint32_t i = 0; i < got; i++) if (mem[i]) pFree(dev, mem[i], NULL);
    free(mem);

    uint64_t b2 = 0, u2 = 0;
    query_budget(c, pGMP2, heap, &b2, &u2);
    ju64("budget_after_free_bytes", b2);
    ju64("usage_after_free_bytes", u2);

    /* Does usage actually track our allocations? If not, the budget is
     * decorative and Stage 7 cannot lean on it. */
    jbool("usage_tracked_allocations", c->have_budget && u1 > u0);
    ju64("headroom_at_peak_bytes", b1 > u1 ? b1 - u1 : 0);
    /* The initial budget is an estimate, not a ceiling: exceeding it is
     * normal and expected. Recording it makes that explicit in the data. */
    jbool("exceeded_initial_budget",
          (uint64_t)got * chunk_mb * 1024ull * 1024ull > b0);

    jobj_end();
}

/* ------------------------------------------------------------------ */
/* TEST 4 -- what the single queue actually costs                      */
/* ------------------------------------------------------------------ */

/*
 * Stage 4's hypothesis says D3D12's copy queue cannot overlap the direct
 * queue here, because there is only one. This turns that into a number.
 *
 * Three measurements on the same queue:
 *   R  compute dispatches alone      (stands in for frame render work)
 *   U  buffer copies alone           (stands in for texture streaming)
 *   I  both, interleaved
 *
 * If I == R + U the two cannot overlap at all and the copies cost full
 * wall-clock time. If I < R + U the driver is pipelining them despite the
 * single queue, and the cost is smaller than the topology suggests.
 *
 * This is a synthetic proxy, not FH6. It bounds the effect; it does not
 * predict the game's frame time.
 */
typedef struct { double wall, gpu; int ok; } Timing;

static void test_queue_cost(Ctx *c, VkDevice dev, uint32_t gfx_family,
                            uint32_t iters, uint32_t groups, uint32_t copy_mb)
{
    jobj("queue_cost");

    DevFn f;
    if (!load_devfn(dev, &f)) {
        jstr("error", "device entry points unavailable"); jobj_end(); return;
    }

    PFN_vkGetPhysicalDeviceMemoryProperties pGMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)(uintptr_t)
        p_GIPA(c->inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    pGMP(c->pd, &mp);

    const VkDeviceSize copy_bytes = (VkDeviceSize)copy_mb * 1024 * 1024;
    Workload w;
    if (!workload_create(&f, dev, &mp, 64, 16u * 1024 * 1024, copy_bytes, &w)) {
        jstr("error", "could not build the workload (allocation failed?)");
        jobj_end(); return;
    }

    VkQueue q = VK_NULL_HANDLE;
    f.GetDeviceQueue(dev, gfx_family, 0, &q);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gfx_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    if (f.CreateCommandPool(dev, &cpci, NULL, &pool) != VK_SUCCESS) {
        jstr("error", "vkCreateCommandPool failed");
        workload_destroy(&f, dev, &w); jobj_end(); return;
    }

    uint32_t ts_bits = c->qf[gfx_family].timestampValidBits;
    VkQueryPool qp = VK_NULL_HANDLE;
    if (ts_bits) {
        VkQueryPoolCreateInfo qpci = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
        };
        f.CreateQueryPool(dev, &qpci, NULL, &qp);
    }

    ju32("iterations", iters);
    ju32("workgroups_per_dispatch", groups);
    ju32("copy_mb_per_iteration", copy_mb);
    jbool("gpu_timestamps", ts_bits != 0);

    Timing res[3];
    static const char *names[3] = { "render_only", "upload_only", "interleaved" };
    emit_gpu_state("gpu_state_before");
    for (int mode = 0; mode < 3; mode++) {
        uint32_t nd = (mode == 1) ? 0 : iters;
        uint32_t nc = (mode == 0) ? 0 : iters;

        VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        /*
         * Each mode is submitted twice and only the second is measured. The
         * modes run in sequence, so without this the first one absorbs all
         * the warm-up -- caches, clocks, first-touch page faults -- and the
         * later ones look artificially fast. A validation run on lavapipe
         * had "interleaved" beating "render_only" while doing strictly more
         * work, which is how this was caught.
         */
        res[mode].ok = 0;
        for (int pass = 0; pass < 2; pass++) {
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (f.AllocateCommandBuffers(dev, &cbai, &cb) != VK_SUCCESS) break;
            VkCommandBufferBeginInfo bi = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            f.BeginCommandBuffer(cb, &bi);
            if (qp) {
                f.CmdResetQueryPool(cb, qp, 0, 2);
                f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
            }
            record(&f, cb, &w, groups, nd, nc, mode == 2);
            if (qp) f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
            f.EndCommandBuffer(cb);

            double wall = 0, gpu = -1;
            int ok = timed_submit(&f, dev, q, cb, qp, c->props.limits.timestampPeriod,
                                  ts_bits, &wall, &gpu);
            if (pass == 1) {
                res[mode].ok = ok;
                res[mode].wall = wall;
                res[mode].gpu = gpu;
            }
        }

        jobj(names[mode]);
        emit_gpu_state("gpu_state");
        if (res[mode].ok) {
            jf("wall_ms", res[mode].wall);
            if (res[mode].gpu >= 0) jf("gpu_ms", res[mode].gpu); else jnull("gpu_ms");
        } else {
            jstr("error", "submit failed");
        }
        jobj_end();
    }

    if (res[0].ok && res[1].ok && res[2].ok) {
        /* GPU time when we have it: it excludes CPU-side submit overhead,
         * which is not what this test is about. */
        int use_gpu = res[0].gpu >= 0 && res[1].gpu >= 0 && res[2].gpu >= 0;
        double r = use_gpu ? res[0].gpu : res[0].wall;
        double u = use_gpu ? res[1].gpu : res[1].wall;
        double serial = r + u;
        double actual = use_gpu ? res[2].gpu : res[2].wall;
        jstr("overlap_basis", use_gpu ? "gpu_ms" : "wall_ms");
        jf("serial_sum_ms", serial);
        jf("interleaved_ms", actual);
        /* 0 = no overlap whatsoever; 1 = uploads were entirely free. */
        double overlap = serial > 0 ? (serial - actual) / serial : 0.0;
        jf("overlap_fraction", overlap < 0 ? 0.0 : overlap);
        jf("upload_cost_ms", actual - r);
        /*
         * Interleaved does strictly more work than render_only, so a negative
         * upload cost cannot happen -- it means the GPU clock moved between
         * the two measurements. The soak showed the clock going 1200 -> 607
         * MHz over ~15 s, which is far slower than the per-mode warm-up pass
         * can absorb. Flag it rather than reporting a number that looks fine.
         */
        int clock_confounded = (actual < r) || overlap > 0.6;
        jbool("clock_confounded", clock_confounded);
        if (clock_confounded)
            jstr("__WARNING",
                 "interleaved came out cheaper than render alone, or overlap "
                 "exceeded 0.6. The GPU clock moved mid-test; treat this "
                 "run's queue numbers as invalid and re-run from a cool "
                 "device.");
        jstr("reading",
             actual > serial * 1.05
             ? "worse than serial: no overlap, and the barriers between "
               "dispatch and copy cost extra on top"
             : overlap < 0.05
             ? "no overlap: uploads cost their full time on top of render"
             : "partial overlap: the driver pipelines despite the single queue");
    } else {
        jnull("overlap_fraction");
    }

    if (qp) f.DestroyQueryPool(dev, qp, NULL);
    f.DestroyCommandPool(dev, pool, NULL);
    workload_destroy(&f, dev, &w);
    jobj_end();
}

/* ------------------------------------------------------------------ */
/* TEST 5 -- GPU frame-time curve                                      */
/* ------------------------------------------------------------------ */

/*
 * Sweeps GPU load and reports measured GPU time per "frame", so the load at
 * which this device falls under a 60 Hz budget can be read off directly.
 *
 * OFFSCREEN AND SYNTHETIC. There is no swapchain, no vsync, no compositor,
 * and compute dispatches are not a game frame. It measures the timestamp
 * path and the GPU's raw throughput curve -- not presented FPS.
 */
static void test_frame_loop(Ctx *c, VkDevice dev, uint32_t gfx_family,
                            uint32_t frames)
{
    jobj("frame_loop");
    jstr("__caveat",
         "offscreen compute, no swapchain or vsync: this is a GPU throughput "
         "curve and the timestamp path, not presented FPS");

    DevFn f;
    if (!load_devfn(dev, &f)) { jstr("error", "device entry points unavailable"); jobj_end(); return; }

    PFN_vkGetPhysicalDeviceMemoryProperties pGMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)(uintptr_t)
        p_GIPA(c->inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    pGMP(c->pd, &mp);

    /* One thread per float, so the storage buffer has to cover the largest
     * dispatch we will issue -- otherwise robustBufferAccess silently drops
     * the out-of-range half and the heavy levels measure less work than they
     * claim to. */
    const uint32_t MAX_GROUPS = 262144;
    const VkDeviceSize work_bytes = (VkDeviceSize)MAX_GROUPS * 64 * sizeof(float);

    Workload w;
    if (!workload_create(&f, dev, &mp, 64, work_bytes, 1u * 1024 * 1024, &w)) {
        jstr("error", "could not build the workload"); jobj_end(); return;
    }

    VkQueue q = VK_NULL_HANDLE;
    f.GetDeviceQueue(dev, gfx_family, 0, &q);
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gfx_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    f.CreateCommandPool(dev, &cpci, NULL, &pool);

    uint32_t ts_bits = c->qf[gfx_family].timestampValidBits;
    VkQueryPool qp = VK_NULL_HANDLE;
    if (ts_bits) {
        VkQueryPoolCreateInfo qpci = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
        };
        f.CreateQueryPool(dev, &qpci, NULL, &qp);
    }

    ju32("frames_per_level", frames);
    jf("timestamp_period_ns", c->props.limits.timestampPeriod);
    jbool("gpu_state_readable", gpu_state_available());
    emit_gpu_state("gpu_state_start");

    /*
     * Doubling sweep rather than a fixed ladder. A ladder calibrated on one
     * device tells you nothing on a faster one: the first Adreno 830 run
     * topped out at 32% of the 60 Hz budget, so every level "fit" and the
     * threshold was never found. This climbs until the budget is actually
     * exceeded, so the crossing point is measured on whatever hardware runs it.
     */
    const double BUDGET_60HZ_MS = 1000.0 / 60.0;
    double prev_groups = 0, prev_ms = 0, threshold = 0;
    double consistent_to = 0;   /* last level whose WORST frame still fit */

    jarr("levels");
    for (uint32_t groups = 64; groups <= MAX_GROUPS; groups *= 4) {
        double gpu_sum = 0, wall_sum = 0;
        double gpu_min = 1e18, gpu_max = 0;
        uint32_t counted = 0;

        for (uint32_t fr = 0; fr < frames; fr++) {
            VkCommandBufferAllocateInfo cbai = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (f.AllocateCommandBuffers(dev, &cbai, &cb) != VK_SUCCESS) break;
            VkCommandBufferBeginInfo bi = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            f.BeginCommandBuffer(cb, &bi);
            if (qp) {
                f.CmdResetQueryPool(cb, qp, 0, 2);
                f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
            }
            record(&f, cb, &w, groups, 1, 0, 0);
            if (qp) f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
            f.EndCommandBuffer(cb);

            double wall = 0, gpu = -1;
            if (!timed_submit(&f, dev, q, cb, qp, c->props.limits.timestampPeriod,
                              ts_bits, &wall, &gpu)) break;
            /* Discard the first iteration: it carries warm-up, not frame cost. */
            if (fr == 0) continue;
            wall_sum += wall; counted++;
            if (gpu >= 0) {
                gpu_sum += gpu;
                if (gpu < gpu_min) gpu_min = gpu;
                if (gpu > gpu_max) gpu_max = gpu;
            }
        }

        jobj(NULL);
        ju32("workgroups", groups);
        ju32("frames_counted", counted);
        emit_gpu_state("gpu_state_after");
        if (counted) {
            jf("wall_ms_avg", wall_sum / counted);
            if (gpu_sum > 0) {
                double avg = gpu_sum / counted;
                jf("gpu_ms_avg", avg);
                jf("gpu_ms_min", gpu_min);
                jf("gpu_ms_max", gpu_max);
                jf("implied_fps", avg > 0 ? 1000.0 / avg : 0.0);
                /* Spread matters more than the average once the GPU is
                 * actually loaded. A level whose mean fits the budget but
                 * whose worst frame does not is not a 60 Hz level -- that is
                 * where the 1% lows live. */
                jf("gpu_ms_spread_pct", avg > 0 ? (gpu_max - gpu_min) / avg * 100.0 : 0.0);
                jf("implied_fps_worst", gpu_max > 0 ? 1000.0 / gpu_max : 0.0);
                jbool("fits_60hz_avg", avg <= BUDGET_60HZ_MS);
                jbool("fits_60hz_worst", gpu_max <= BUDGET_60HZ_MS);
            } else {
                jnull("gpu_ms_avg");
            }
        } else {
            jstr("error", "no frames completed");
        }
        jobj_end();
        fflush(stdout);

        if (counted && gpu_sum > 0) {
            double avg = gpu_sum / counted;
            if (gpu_max <= BUDGET_60HZ_MS) consistent_to = groups;
            if (avg > BUDGET_60HZ_MS) {
                /* Log-linear interpolation between the last two points: work
                 * scales roughly linearly with group count here. */
                if (prev_ms > 0 && avg > prev_ms)
                    threshold = prev_groups +
                        (groups - prev_groups) * (BUDGET_60HZ_MS - prev_ms) / (avg - prev_ms);
                else
                    threshold = groups;
                break;
            }
            prev_groups = groups;
            prev_ms = avg;
        }
    }
    jarr_end();

    jf("groups_consistently_under_60hz", consistent_to);
    jstr("consistency_note",
         "highest level whose WORST frame stayed inside 16.67 ms. Above this "
         "the average can still fit while individual frames miss -- which is "
         "what 1% lows measure.");
    if (threshold > 0) {
        jf("groups_at_60hz_budget", threshold);
        jstr("threshold_note",
             "interpolated workgroup count where GPU time reaches 16.67 ms");
    } else {
        jnull("groups_at_60hz_budget");
        jstr("threshold_note",
             "never exceeded the 60 Hz budget within the sweep cap -- the GPU "
             "is faster than this synthetic workload can load it");
    }

    if (qp) f.DestroyQueryPool(dev, qp, NULL);
    f.DestroyCommandPool(dev, pool, NULL);
    workload_destroy(&f, dev, &w);
    jobj_end();
}

/* ------------------------------------------------------------------ */
/* TEST 6 -- sustained load (soak)                                     */
/* ------------------------------------------------------------------ */

/*
 * Everything above runs for a few seconds and then stops, which turned out to
 * be why four runs disagreed by up to 1.6x. A profiler trace over one of them
 * showed the GPU at 0% busy and 222 MHz for almost the whole window, with the
 * die temperature falling 49.2C -> 38.4C from start to finish. The benchmark
 * never heated anything: the variance came from whatever the phone had been
 * doing BEFORE the run, and four seconds is nowhere near steady state.
 *
 * This holds one load continuously and records every frame, so the throttling
 * curve shows up in the frame times themselves. That needs no sysfs access
 * (blocked by Winlator's proot) and no external sampler (1 Hz cannot see a
 * four-second burst).
 */
/*
 * Runs each load back to back without a cool-down. That is deliberate: a game
 * keeps the device hot, so the interesting figure is what each load sustains
 * on an already-warm phone, not what it manages from cold.
 */
static void soak_one(Ctx *c, VkDevice dev, uint32_t gfx_family,
                     uint32_t seconds, uint32_t groups,
                     double *out_sustained_ms)
{
    ju32("seconds_requested", seconds);
    ju32("workgroups", groups);

    DevFn f;
    if (!load_devfn(dev, &f)) { jstr("error", "device entry points unavailable"); return; }

    PFN_vkGetPhysicalDeviceMemoryProperties pGMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)(uintptr_t)
        p_GIPA(c->inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    pGMP(c->pd, &mp);

    const VkDeviceSize work_bytes = (VkDeviceSize)groups * 64 * sizeof(float);
    Workload w;
    if (!workload_create(&f, dev, &mp, 64, work_bytes, 1u * 1024 * 1024, &w)) {
        jstr("error", "could not build the workload"); return;
    }

    VkQueue q = VK_NULL_HANDLE;
    f.GetDeviceQueue(dev, gfx_family, 0, &q);
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gfx_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    f.CreateCommandPool(dev, &cpci, NULL, &pool);

    uint32_t ts_bits = c->qf[gfx_family].timestampValidBits;
    VkQueryPool qp = VK_NULL_HANDLE;
    if (ts_bits) {
        VkQueryPoolCreateInfo qpci = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2,
        };
        f.CreateQueryPool(dev, &qpci, NULL, &qp);
    }

    /* Bucket on the fly rather than keeping every frame: a two-minute soak is
     * thousands of frames and the shape is what matters, not the raw list. */
    #define SOAK_BUCKETS 24
    const double bucket_s = seconds / (double)SOAK_BUCKETS;
    struct { uint32_t n; double sum, min, max; } b[SOAK_BUCKETS];
    for (int i = 0; i < SOAK_BUCKETS; i++) { b[i].n = 0; b[i].sum = 0; b[i].min = 1e18; b[i].max = 0; }

    double t_start = now_ms();
    double deadline = t_start + seconds * 1000.0;
    uint32_t total_frames = 0;

    while (now_ms() < deadline) {
        VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (f.AllocateCommandBuffers(dev, &cbai, &cb) != VK_SUCCESS) break;
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        f.BeginCommandBuffer(cb, &bi);
        if (qp) {
            f.CmdResetQueryPool(cb, qp, 0, 2);
            f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        }
        record(&f, cb, &w, groups, 1, 0, 0);
        if (qp) f.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        f.EndCommandBuffer(cb);

        double wall = 0, gpu = -1;
        if (!timed_submit(&f, dev, q, cb, qp, c->props.limits.timestampPeriod,
                          ts_bits, &wall, &gpu)) break;

        double elapsed_s = (now_ms() - t_start) / 1000.0;
        int idx = (int)(elapsed_s / bucket_s);
        if (idx < 0) idx = 0;
        if (idx >= SOAK_BUCKETS) idx = SOAK_BUCKETS - 1;
        double v = gpu >= 0 ? gpu : wall;
        b[idx].n++; b[idx].sum += v;
        if (v < b[idx].min) b[idx].min = v;
        if (v > b[idx].max) b[idx].max = v;
        total_frames++;
    }

    double actual_s = (now_ms() - t_start) / 1000.0;
    jf("seconds_actual", actual_s);
    ju32("frames", total_frames);
    jbool("gpu_timestamps", ts_bits != 0);

    jarr("buckets");
    for (int i = 0; i < SOAK_BUCKETS; i++) {
        if (!b[i].n) continue;
        jobj(NULL);
        jf("t_start_s", i * bucket_s);
        ju32("frames", b[i].n);
        jf("ms_avg", b[i].sum / b[i].n);
        jf("ms_min", b[i].min);
        jf("ms_max", b[i].max);
        jobj_end();
    }
    jarr_end();

    int first = -1, last = -1;
    for (int i = 0; i < SOAK_BUCKETS; i++) if (b[i].n) { if (first < 0) first = i; last = i; }

    /* Sustained cost is the mean over the LAST THIRD of the run. The first
     * seconds are boost clock and say nothing about what a game will see;
     * a single trailing bucket is too few frames to be stable. */
    double sustained = 0;
    if (first >= 0) {
        int from = first + (last - first) * 2 / 3;
        double sum = 0; uint32_t n = 0;
        for (int i = from; i <= last; i++) { sum += b[i].sum; n += b[i].n; }
        if (n) sustained = sum / n;
    }
    jf("sustained_ms", sustained);
    if (out_sustained_ms) *out_sustained_ms = sustained;

    if (first >= 0 && last > first) {
        double a = b[first].sum / b[first].n;
        /*
         * Peak is the FASTEST bucket, not the first. The first bucket carries
         * allocation and first-submit outliers -- measured maxima of 32 ms and
         * 65 ms against steady-state means of 7 ms and 17 ms -- which inflate
         * it enough to make a degrading level report as improving.
         */
        double peak = 1e18;
        for (int i = first; i <= last; i++)
            if (b[i].n) { double m = b[i].sum / b[i].n; if (m < peak) peak = m; }
        jf("first_bucket_ms", a);
        jf("last_bucket_ms", b[last].sum / b[last].n);
        jf("peak_ms", peak);
        a = peak;
        jf("degradation_x", a > 0 ? sustained / a : 0.0);
        jf("degradation_pct", a > 0 ? (sustained - a) / a * 100.0 : 0.0);
        jf("sustained_fps", sustained > 0 ? 1000.0 / sustained : 0.0);
        jbool("sustained_fits_60hz", sustained > 0 && sustained <= 1000.0 / 60.0);
        jstr("reading",
             sustained > a * 1.15 ? "degrades under sustained load -- thermal or DVFS"
             : sustained < a * 0.95 ? "IMPROVES under load -- clocks ramping up, not throttling"
             : "holds steady across the soak");
    } else {
        jnull("degradation_x");
    }

    if (qp) f.DestroyQueryPool(dev, qp, NULL);
    f.DestroyCommandPool(dev, pool, NULL);
    workload_destroy(&f, dev, &w);
}

/*
 * The planning number this project actually needs: the load the GPU can hold
 * inside a 60 Hz budget *after* it has throttled, rather than during the
 * first fifteen seconds of boost.
 */
static void test_soak_ladder(Ctx *c, VkDevice dev, uint32_t gfx_family,
                             uint32_t seconds, const uint32_t *loads, int nloads)
{
    jobj("soak");
    ju32("seconds_per_level", seconds);
    jstr("__note",
         "levels run back to back with no cool-down, because a game keeps the "
         "device hot. sustained_ms is the mean over each level's last third.");

    double prev_g = 0, prev_ms = 0, threshold = 0;

    jarr("levels");
    for (int i = 0; i < nloads; i++) {
        double sustained = 0;
        jobj(NULL);
        soak_one(c, dev, gfx_family, seconds, loads[i], &sustained);
        jobj_end();
        fflush(stdout);

        const double budget = 1000.0 / 60.0;
        if (sustained > 0) {
            if (sustained > budget && prev_ms > 0 && prev_ms <= budget) {
                threshold = prev_g + (loads[i] - prev_g) *
                            (budget - prev_ms) / (sustained - prev_ms);
            }
            prev_g = loads[i];
            prev_ms = sustained;
        }
    }
    jarr_end();

    if (threshold > 0) {
        jf("sustained_groups_at_60hz", threshold);
        jstr("threshold_note",
             "workgroups the GPU holds inside 16.67 ms AFTER throttling. This "
             "is the planning number; frame_loop's threshold is boost clock.");
    } else {
        jnull("sustained_groups_at_60hz");
        jstr("threshold_note",
             "60 Hz budget not bracketed by these levels -- widen --soak-loads");
    }
    jobj_end();
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage_text(void)
{
    fputs(
      "vkbench " BENCH_VERSION " -- Star Bionic Vulkan measurement harness\n"
      "\n"
      "  --lib PATH        load this Vulkan loader\n"
      "  --device N        physical device index (default 0)\n"
      "  --pipelines N     pipelines to compile (default 48)\n"
      "  --chunk-mb N      allocation chunk (default 128)\n"
      "  --cap-mb N        allocation cap (default 10240)\n"
      "  --headroom-floor-mb N  stop with this much headroom left\n"
      "                    (default 1024; below this Android kills the\n"
      "                     container rather than failing the allocation)\n"
      "  --skip-memory     do not run the allocation test\n"
      "  --skip-gpu        do not run the queue-cost / frame-loop tests\n"
      "  --qc-iters N      queue-cost iterations (default 32)\n"
      "  --frames N        frames per load level (default 9)\n"
      "  --soak N          seconds per soak level (default 45, 0 = off)\n"
      "  --soak-loads A,B,C  soak these loads in turn (default 16384,65536,131072)\n"
      "  --out FILE        write JSON here instead of stdout\n"
      "  -h, --help        this text\n",
      stderr);
}

int main(int argc, char **argv)
{
    const char *libpath = NULL, *outpath = NULL;
    uint32_t devidx = 0, npipe = DEFAULT_PIPELINES;
    uint32_t chunk_mb = DEFAULT_ALLOC_CHUNK_MB, cap_mb = DEFAULT_ALLOC_CAP_MB;
    uint32_t floor_mb = DEFAULT_HEADROOM_FLOOR_MB;
    uint32_t qc_iters = 32, qc_groups = 1024, qc_copy_mb = 8, frames = 9;
    /* 60 s at a load near the 60 Hz threshold: long enough to reach steady
     * state, which four seconds is not. */
    /* 45 s is past the ~37 s at which throttling settled when measured. Three
     * levels bracket the 60 Hz budget at sustained clock without running for
     * an unreasonable time. */
    uint32_t soak_s = 45;
    uint32_t soak_loads[8] = { 16384, 65536, 131072 };
    int n_soak_loads = 3;
    int skip_gpu = 0;
    int skip_memory = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lib") && i + 1 < argc) libpath = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) devidx = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--pipelines") && i + 1 < argc) npipe = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--chunk-mb") && i + 1 < argc) chunk_mb = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--cap-mb") && i + 1 < argc) cap_mb = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--headroom-floor-mb") && i + 1 < argc) floor_mb = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--skip-memory")) skip_memory = 1;
        else if (!strcmp(argv[i], "--skip-gpu")) skip_gpu = 1;
        else if (!strcmp(argv[i], "--qc-iters") && i + 1 < argc) qc_iters = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--soak") && i + 1 < argc) soak_s = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--soak-loads") && i + 1 < argc) {
            n_soak_loads = 0;
            for (char *tok = strtok(argv[++i], ","); tok && n_soak_loads < 8;
                 tok = strtok(NULL, ","))
                soak_loads[n_soak_loads++] = (uint32_t)strtoul(tok, NULL, 10);
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage_text(); return 0; }
        else { fprintf(stderr, "vkbench: unknown argument '%s'\n", argv[i]); usage_text(); return 2; }
    }

    if (chunk_mb == 0) chunk_mb = DEFAULT_ALLOC_CHUNK_MB;

#ifdef _WIN32
    /* Winlator launches by tap: no shell, no redirection. Same reasoning as
     * vkprobe -- write beside the exe so the file is findable. */
    static char defout[MAX_PATH];
    if (!outpath) {
        DWORD n = GetModuleFileNameA(NULL, defout, (DWORD)sizeof defout);
        DWORD cut = 0;
        for (DWORD i = n; i > 0 && n < sizeof defout; i--)
            if (defout[i-1] == '\\' || defout[i-1] == '/') { cut = i; break; }
        if (cut && cut + 20 < sizeof defout) {
            strcpy(defout + cut, "vkbench-result.json");
            outpath = defout;
        } else {
            outpath = "vkbench-result.json";
        }
    }
#endif

    if (outpath && !freopen(outpath, "w", stdout)) {
        fprintf(stderr, "vkbench: cannot open --out '%s'\n", outpath);
        return 1;
    }

    if (!load_loader(libpath)) return 1;

    PFN_vkEnumerateInstanceVersion pEIV =
        (PFN_vkEnumerateInstanceVersion)(uintptr_t)p_GIPA(NULL, "vkEnumerateInstanceVersion");
    uint32_t api = VK_API_VERSION_1_0;
    if (pEIV) pEIV(&api);

    PFN_vkCreateInstance pCI =
        (PFN_vkCreateInstance)(uintptr_t)p_GIPA(NULL, "vkCreateInstance");
    if (!pCI) { fprintf(stderr, "vkbench: no vkCreateInstance\n"); return 1; }

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "star-bionic-vkbench",
                             .apiVersion = api };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = pCI(&ici, NULL, &inst);

    jneed[0] = 0;
    jobj(NULL);
    jstr("bench_version", BENCH_VERSION);
    jstr("loader_path", g_libpath ? g_libpath : "(default)");
    {
        char v[32];
        snprintf(v, sizeof v, "%u.%u.%u", VK_API_VERSION_MAJOR(api),
                 VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
        jstr("instance_api_version", v);
    }

    if (r != VK_SUCCESS) {
        jstr("error", "vkCreateInstance failed");
        jobj_end(); fputc('\n', stdout);
        fprintf(stderr, "vkbench: vkCreateInstance failed (%d)\n", r);
        return 1;
    }

    Ctx c;
    memset(&c, 0, sizeof c);
    c.inst = inst;

    PFN_vkEnumeratePhysicalDevices pEPD = IFN(EnumeratePhysicalDevices);
    uint32_t ndev = 0;
    pEPD(inst, &ndev, NULL);
    if (devidx >= ndev) {
        jstr("error", "device index out of range");
        jobj_end(); fputc('\n', stdout);
        return 1;
    }
    VkPhysicalDevice *pds = calloc(ndev, sizeof *pds);
    pEPD(inst, &ndev, pds);
    c.pd = pds[devidx];

    PFN_vkGetPhysicalDeviceProperties pGPDP = IFN(GetPhysicalDeviceProperties);
    pGPDP(c.pd, &c.props);
    jstr("device", c.props.deviceName);
    jbool("kgsl_sysfs_readable", gpu_state_available());
    emit_gpu_state("gpu_state_at_start");

    PFN_vkGetPhysicalDeviceQueueFamilyProperties pQF = IFN(GetPhysicalDeviceQueueFamilyProperties);
    pQF(c.pd, &c.qf_count, NULL);
    c.qf = calloc(c.qf_count, sizeof *c.qf);
    pQF(c.pd, &c.qf_count, c.qf);

    PFN_vkEnumerateDeviceExtensionProperties pEDE = IFN(EnumerateDeviceExtensionProperties);
    pEDE(c.pd, NULL, &c.ext_count, NULL);
    c.ext = calloc(c.ext_count ? c.ext_count : 1, sizeof *c.ext);
    pEDE(c.pd, NULL, &c.ext_count, c.ext);

    c.have_feedback = has_ext(c.ext, c.ext_count, "VK_EXT_pipeline_creation_feedback") ||
                      c.props.apiVersion >= VK_API_VERSION_1_3;
    c.have_budget = has_ext(c.ext, c.ext_count, "VK_EXT_memory_budget");

    /* ---- test 1 needs no device ---- */
    test_queues(&c);

    /* ---- create a device for tests 2 and 3 ---- */
    int gfx = -1;
    for (uint32_t i = 0; i < c.qf_count; i++)
        if (c.qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { gfx = (int)i; break; }
    if (gfx < 0) {
        jstr("error", "no compute-capable queue family");
        jobj_end(); fputc('\n', stdout);
        return 1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)gfx, .queueCount = 1, .pQueuePriorities = &prio,
    };
    const char *dev_exts[4];
    uint32_t nde = 0;
    if (c.have_budget) dev_exts[nde++] = "VK_EXT_memory_budget";
    if (has_ext(c.ext, c.ext_count, "VK_EXT_pipeline_creation_feedback"))
        dev_exts[nde++] = "VK_EXT_pipeline_creation_feedback";

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nde, .ppEnabledExtensionNames = nde ? dev_exts : NULL,
    };
    PFN_vkCreateDevice pCD = IFN(CreateDevice);
    VkDevice dev = VK_NULL_HANDLE;
    VkResult dr = pCD(c.pd, &dci, NULL, &dev);
    if (dr != VK_SUCCESS) {
        jstr("error", "vkCreateDevice failed");
        jobj_end(); fputc('\n', stdout);
        fprintf(stderr, "vkbench: vkCreateDevice failed (%d)\n", dr);
        return 1;
    }
    p_GDPA = IFN(GetDeviceProcAddr);

    test_pipelines(&c, dev, npipe);
    if (!skip_gpu) {
        test_queue_cost(&c, dev, (uint32_t)gfx, qc_iters, qc_groups, qc_copy_mb);
        test_frame_loop(&c, dev, (uint32_t)gfx, frames);
        if (soak_s && n_soak_loads)
            test_soak_ladder(&c, dev, (uint32_t)gfx, soak_s, soak_loads, n_soak_loads);
        else jnull("soak");
    } else {
        jnull("queue_cost");
        jnull("frame_loop");
        jnull("soak");
    }
    if (!skip_memory) test_memory(&c, dev, chunk_mb, cap_mb, floor_mb);
    else jnull("memory");

    PFN_vkDestroyDevice pDD = DFN(DestroyDevice);
    if (pDD) pDD(dev, NULL);

    jobj_end();
    fputc('\n', stdout);

    free(pds); free(c.qf); free(c.ext);
    return 0;
}

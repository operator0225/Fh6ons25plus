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
static void test_memory(Ctx *c, VkDevice dev, uint32_t chunk_mb, uint32_t cap_mb)
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

        /* Report every 8th step; the full curve is noise, the shape is not. */
        if ((got % 8) == 0 || got == 1) {
            uint64_t b = 0, u = 0;
            query_budget(c, pGMP2, heap, &b, &u);
            jobj(NULL);
            ju32("allocated_mb", got * chunk_mb);
            ju64("budget_bytes", b);
            ju64("usage_bytes", u);
            /* budget is usage + estimated remaining, so it RISES as you
             * allocate. Headroom is the figure that actually falls, and the
             * one that predicts where allocation stops. */
            ju64("headroom_bytes", b > u ? b - u : 0);
            jobj_end();
        }
    }
    jarr_end();

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
      "  --skip-memory     do not run the allocation test\n"
      "  --out FILE        write JSON here instead of stdout\n"
      "  -h, --help        this text\n",
      stderr);
}

int main(int argc, char **argv)
{
    const char *libpath = NULL, *outpath = NULL;
    uint32_t devidx = 0, npipe = DEFAULT_PIPELINES;
    uint32_t chunk_mb = DEFAULT_ALLOC_CHUNK_MB, cap_mb = DEFAULT_ALLOC_CAP_MB;
    int skip_memory = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lib") && i + 1 < argc) libpath = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) devidx = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--pipelines") && i + 1 < argc) npipe = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--chunk-mb") && i + 1 < argc) chunk_mb = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--cap-mb") && i + 1 < argc) cap_mb = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--skip-memory")) skip_memory = 1;
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
    if (!skip_memory) test_memory(&c, dev, chunk_mb, cap_mb);
    else jnull("memory");

    PFN_vkDestroyDevice pDD = DFN(DestroyDevice);
    if (pDD) pDD(dev, NULL);

    jobj_end();
    fputc('\n', stdout);

    free(pds); free(c.qf); free(c.ext);
    return 0;
}

/* SPDX-License-Identifier: MIT
 *
 * vkprobe -- Star Bionic Vulkan capability probe (Stage 2)
 *
 * Reports what a Vulkan implementation ACTUALLY exposes, as returned by
 * vkGetPhysicalDeviceFeatures2 / vkGetPhysicalDeviceProperties2 /
 * vkGetPhysicalDeviceFormatProperties2. Nothing here is inferred from the
 * device name, the GPU model, or from documentation.
 *
 * Design rules (these matter, do not "simplify" them away):
 *
 *   1. An extension-gated struct is chained ONLY if the device actually
 *      advertises that extension, or if the core API version promoted it.
 *      Chaining an unsupported struct means the driver never writes to it and
 *      we would report our own zero-init as if it were a driver answer.
 *   2. Every gated block carries a "__source" field naming WHERE the answer
 *      came from ("core1.3", "VK_EXT_mesh_shader", ...) or "not-available"
 *      when the block could not be queried at all. "not queried" and
 *      "queried, returned false" are never conflated.
 *   3. The loader is dlopen'd, so the caller can point this at a specific ICD
 *      (Turnip vs. the Qualcomm blob) and find out which one really answered.
 *      See driver_properties.driverName / driverID in the output.
 *
 * Build (host):    cc -O2 -o vkprobe vkprobe.c -ldl
 * Build (Android): see build-android.sh
 * Run:             ./vkprobe [--lib PATH] [--device N] [--icd PATH]
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
  #define SETENV(k,v)  _putenv_s((k), (v))
#else
  #include <dlfcn.h>
  #define LIB_HANDLE   void *
  #define LIB_OPEN(p)  dlopen((p), RTLD_NOW | RTLD_LOCAL)
  #define LIB_SYM(h,n) dlsym((h), (n))
  #define LIB_ERR()    dlerror()
  #define SETENV(k,v)  setenv((k), (v), 1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PROBE_VERSION "0.1.0"

/* ------------------------------------------------------------------ */
/* Minimal JSON writer                                                 */
/* ------------------------------------------------------------------ */

static int jdepth;
static int jneed[64];

static void jind(void)
{
    for (int i = 0; i < jdepth; i++)
        fputs("  ", stdout);
}

static void jsep(void)
{
    if (jneed[jdepth])
        fputc(',', stdout);
    jneed[jdepth] = 1;
    fputc('\n', stdout);
    jind();
}

static void jesc(const char *s)
{
    fputc('"', stdout);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout);  break;
            case '\r': fputs("\\r", stdout);  break;
            case '\t': fputs("\\t", stdout);  break;
            default:
                if (*p < 0x20)
                    printf("\\u%04x", *p);
                else
                    fputc(*p, stdout);
            }
        }
    }
    fputc('"', stdout);
}

static void jobj(const char *k)
{
    jsep();
    if (k) { jesc(k); fputs(": ", stdout); }
    fputc('{', stdout);
    jdepth++;
    jneed[jdepth] = 0;
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
    fputc('[', stdout);
    jdepth++;
    jneed[jdepth] = 0;
}

static void jarr_end(void)
{
    int empty = !jneed[jdepth];
    jdepth--;
    if (!empty) { fputc('\n', stdout); jind(); }
    fputc(']', stdout);
}

static void jbool(const char *k, VkBool32 v)
{
    jsep(); jesc(k); fputs(": ", stdout); fputs(v ? "true" : "false", stdout);
}

static void ju32(const char *k, uint32_t v)
{
    jsep(); jesc(k); printf(": %u", v);
}

static void ju64(const char *k, uint64_t v)
{
    jsep(); jesc(k); printf(": %llu", (unsigned long long)v);
}

static void jf32(const char *k, float v)
{
    jsep(); jesc(k); printf(": %g", (double)v);
}

static void jstr(const char *k, const char *v)
{
    jsep(); jesc(k); fputs(": ", stdout); jesc(v);
}

static void jnull(const char *k)
{
    jsep(); jesc(k); fputs(": null", stdout);
}

static void jelem_str(const char *v)
{
    jsep(); jesc(v);
}

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

static PFN_vkGetInstanceProcAddr                    p_GetInstanceProcAddr;
static PFN_vkEnumerateInstanceVersion               p_EnumerateInstanceVersion;
static PFN_vkEnumerateInstanceExtensionProperties   p_EnumerateInstanceExtensionProperties;
static PFN_vkEnumerateInstanceLayerProperties       p_EnumerateInstanceLayerProperties;
static PFN_vkCreateInstance                         p_CreateInstance;
static PFN_vkDestroyInstance                        p_DestroyInstance;
static PFN_vkEnumeratePhysicalDevices               p_EnumeratePhysicalDevices;
static PFN_vkEnumerateDeviceExtensionProperties     p_EnumerateDeviceExtensionProperties;
static PFN_vkGetPhysicalDeviceProperties            p_GetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceProperties2           p_GetPhysicalDeviceProperties2;
static PFN_vkGetPhysicalDeviceFeatures2             p_GetPhysicalDeviceFeatures2;
static PFN_vkGetPhysicalDeviceMemoryProperties2     p_GetPhysicalDeviceMemoryProperties2;
static PFN_vkGetPhysicalDeviceFormatProperties2     p_GetPhysicalDeviceFormatProperties2;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties2 p_GetPhysicalDeviceQueueFamilyProperties2;
static PFN_vkGetPhysicalDeviceExternalSemaphoreProperties p_GetPhysicalDeviceExternalSemaphoreProperties;

#define GIPA(inst, name) p_GetInstanceProcAddr(inst, name)

static const char *g_libpath;
static int g_software_rasterizer;  /* set if any device is a CPU renderer */

static int load_loader(const char *path)
{
    /*
     * Order matters on Android. Termux ships its own libvulkan under its
     * prefix, backed by a software rasterizer (lavapipe/llvmpipe). A bare
     * "libvulkan.so" resolves to THAT, and the probe then cheerfully reports
     * a CPU renderer's capabilities as if they were the GPU's. Ask the
     * Android system loader first -- it is the one that reaches the vendor
     * HAL, and therefore the actual Adreno.
     */
    static const char *candidates[] = {
#ifdef _WIN32
        /* Inside Wine this is winevulkan, which forwards to whatever native
         * driver the container is configured with -- i.e. exactly the device
         * VKD3D-Proton will see. That is the point of the Windows build. */
        "vulkan-1.dll",
#else
        "/system/lib64/libvulkan.so",
        "/system/lib/libvulkan.so",
        "libvulkan.so.1",
        "libvulkan.so",
#endif
        NULL,
    };
    LIB_HANDLE lib = NULL;

    if (path) {
        lib = LIB_OPEN(path);
        g_libpath = path;
    } else {
        for (int i = 0; candidates[i]; i++) {
            lib = LIB_OPEN(candidates[i]);
            if (lib) { g_libpath = candidates[i]; break; }
        }
    }
    if (!lib) {
        fprintf(stderr, "vkprobe: cannot load Vulkan loader: %s\n", LIB_ERR());
        return 0;
    }

    p_GetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)(uintptr_t)LIB_SYM(lib, "vkGetInstanceProcAddr");
    if (!p_GetInstanceProcAddr) {
        fprintf(stderr, "vkprobe: no vkGetInstanceProcAddr in %s\n", g_libpath);
        return 0;
    }

    p_EnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)GIPA(NULL, "vkEnumerateInstanceVersion");
    p_EnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)GIPA(NULL, "vkEnumerateInstanceExtensionProperties");
    p_EnumerateInstanceLayerProperties =
        (PFN_vkEnumerateInstanceLayerProperties)GIPA(NULL, "vkEnumerateInstanceLayerProperties");
    p_CreateInstance =
        (PFN_vkCreateInstance)GIPA(NULL, "vkCreateInstance");

    return p_CreateInstance != NULL;
}

/* ------------------------------------------------------------------ */
/* Extension bookkeeping                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    VkExtensionProperties *e;
    uint32_t n;
} ExtList;

static int ext_has(const ExtList *l, const char *name)
{
    for (uint32_t i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].extensionName, name))
            return 1;
    return 0;
}

static uint32_t ext_rev(const ExtList *l, const char *name)
{
    for (uint32_t i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].extensionName, name))
            return l->e[i].specVersion;
    return 0;
}

/*
 * Decide where a capability block comes from. `core` is the API version that
 * promoted it to core (0 if never promoted). Returns:
 *   "core1.x"       -> query via the promoted core struct
 *   "<ext name>"    -> query via the extension struct
 *   "not-available" -> do not chain anything
 */
static const char *src_of(uint32_t api, uint32_t core, const ExtList *l, const char *ext)
{
    if (core && api >= core) {
        if (core == VK_API_VERSION_1_1) return "core1.1";
        if (core == VK_API_VERSION_1_2) return "core1.2";
        if (core == VK_API_VERSION_1_3) return "core1.3";
        return "core";
    }
    if (ext && ext_has(l, ext)) return ext;
    return "not-available";
}

static int avail(const char *s) { return strcmp(s, "not-available") != 0; }

/* ------------------------------------------------------------------ */
/* Chain helper                                                        */
/* ------------------------------------------------------------------ */

static void chain(void *head, void *next)
{
    VkBaseOutStructure *h = (VkBaseOutStructure *)head;
    VkBaseOutStructure *n = (VkBaseOutStructure *)next;
    n->pNext = h->pNext;
    h->pNext = n;
}

/* ------------------------------------------------------------------ */
/* Formats                                                             */
/* ------------------------------------------------------------------ */

typedef struct { VkFormat fmt; const char *name; const char *group; } FmtEntry;

static const FmtEntry g_formats[] = {
    /* BCn -- what every DX12 PC title ships its textures in. If these are not
     * natively supported, VKD3D-Proton must transcode or decompress, which is
     * a large CPU and memory cost. This is the single most important format
     * question for a DX12 title on a mobile GPU. */
    { VK_FORMAT_BC1_RGB_UNORM_BLOCK,   "BC1_RGB_UNORM",   "bc" },
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,  "BC1_RGBA_UNORM",  "bc" },
    { VK_FORMAT_BC1_RGBA_SRGB_BLOCK,   "BC1_RGBA_SRGB",   "bc" },
    { VK_FORMAT_BC2_UNORM_BLOCK,       "BC2_UNORM",       "bc" },
    { VK_FORMAT_BC3_UNORM_BLOCK,       "BC3_UNORM",       "bc" },
    { VK_FORMAT_BC3_SRGB_BLOCK,        "BC3_SRGB",        "bc" },
    { VK_FORMAT_BC4_UNORM_BLOCK,       "BC4_UNORM",       "bc" },
    { VK_FORMAT_BC5_UNORM_BLOCK,       "BC5_UNORM",       "bc" },
    { VK_FORMAT_BC6H_UFLOAT_BLOCK,     "BC6H_UFLOAT",     "bc" },
    { VK_FORMAT_BC7_UNORM_BLOCK,       "BC7_UNORM",       "bc" },
    { VK_FORMAT_BC7_SRGB_BLOCK,        "BC7_SRGB",        "bc" },

    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK,  "ASTC_4x4_UNORM",  "astc" },
    { VK_FORMAT_ASTC_6x6_UNORM_BLOCK,  "ASTC_6x6_UNORM",  "astc" },
    { VK_FORMAT_ASTC_8x8_UNORM_BLOCK,  "ASTC_8x8_UNORM",  "astc" },
    { VK_FORMAT_ASTC_8x8_SRGB_BLOCK,   "ASTC_8x8_SRGB",   "astc" },

    { VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_R8G8B8A8_UNORM", "etc2" },

    { VK_FORMAT_D16_UNORM,             "D16_UNORM",           "depth" },
    { VK_FORMAT_X8_D24_UNORM_PACK32,   "X8_D24_UNORM_PACK32", "depth" },
    { VK_FORMAT_D24_UNORM_S8_UINT,     "D24_UNORM_S8_UINT",   "depth" },
    { VK_FORMAT_D32_SFLOAT,            "D32_SFLOAT",          "depth" },
    { VK_FORMAT_D32_SFLOAT_S8_UINT,    "D32_SFLOAT_S8_UINT",  "depth" },
    { VK_FORMAT_S8_UINT,               "S8_UINT",             "depth" },

    { VK_FORMAT_R8G8B8A8_UNORM,        "R8G8B8A8_UNORM",   "color" },
    { VK_FORMAT_R8G8B8A8_SRGB,         "R8G8B8A8_SRGB",    "color" },
    { VK_FORMAT_B8G8R8A8_UNORM,        "B8G8R8A8_UNORM",   "color" },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM", "color" },
    { VK_FORMAT_R16G16B16A16_SFLOAT,   "R16G16B16A16_SFLOAT", "color" },
    { VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT", "color" },
    { VK_FORMAT_R32G32B32A32_SFLOAT,   "R32G32B32A32_SFLOAT", "color" },

    { VK_FORMAT_R8_UINT,               "R8_UINT",          "integer" },
    { VK_FORMAT_R16_UINT,              "R16_UINT",         "integer" },
    { VK_FORMAT_R32_UINT,              "R32_UINT",         "integer" },
    { VK_FORMAT_R32G32B32A32_UINT,     "R32G32B32A32_UINT","integer" },
};

static void emit_format_flags(const char *key, VkFormatFeatureFlags2 f)
{
    jarr(key);
    if (f & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)                jelem_str("SAMPLED_IMAGE");
    if (f & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT)                jelem_str("STORAGE_IMAGE");
    if (f & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_ATOMIC_BIT)         jelem_str("STORAGE_IMAGE_ATOMIC");
    if (f & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)             jelem_str("COLOR_ATTACHMENT");
    if (f & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT)       jelem_str("COLOR_ATTACHMENT_BLEND");
    if (f & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)     jelem_str("DEPTH_STENCIL_ATTACHMENT");
    if (f & VK_FORMAT_FEATURE_2_BLIT_SRC_BIT)                     jelem_str("BLIT_SRC");
    if (f & VK_FORMAT_FEATURE_2_BLIT_DST_BIT)                     jelem_str("BLIT_DST");
    if (f & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT)  jelem_str("SAMPLED_IMAGE_FILTER_LINEAR");
    if (f & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT)                 jelem_str("TRANSFER_SRC");
    if (f & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT)                 jelem_str("TRANSFER_DST");
    jarr_end();
}

/* ------------------------------------------------------------------ */
/* Enum -> string                                                      */
/* ------------------------------------------------------------------ */

static const char *driver_id_str(VkDriverId id)
{
    switch (id) {
    case VK_DRIVER_ID_AMD_PROPRIETARY:            return "AMD_PROPRIETARY";
    case VK_DRIVER_ID_AMD_OPEN_SOURCE:            return "AMD_OPEN_SOURCE";
    case VK_DRIVER_ID_MESA_RADV:                  return "MESA_RADV";
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:         return "NVIDIA_PROPRIETARY";
    case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS:  return "INTEL_PROPRIETARY_WINDOWS";
    case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:     return "INTEL_OPEN_SOURCE_MESA";
    case VK_DRIVER_ID_IMAGINATION_PROPRIETARY:    return "IMAGINATION_PROPRIETARY";
    case VK_DRIVER_ID_QUALCOMM_PROPRIETARY:       return "QUALCOMM_PROPRIETARY";
    case VK_DRIVER_ID_ARM_PROPRIETARY:            return "ARM_PROPRIETARY";
    case VK_DRIVER_ID_GOOGLE_SWIFTSHADER:         return "GOOGLE_SWIFTSHADER";
    case VK_DRIVER_ID_GGP_PROPRIETARY:            return "GGP_PROPRIETARY";
    case VK_DRIVER_ID_BROADCOM_PROPRIETARY:       return "BROADCOM_PROPRIETARY";
    case VK_DRIVER_ID_MESA_LLVMPIPE:              return "MESA_LLVMPIPE";
    case VK_DRIVER_ID_MOLTENVK:                   return "MOLTENVK";
    case VK_DRIVER_ID_MESA_TURNIP:                return "MESA_TURNIP";
    case VK_DRIVER_ID_MESA_PANVK:                 return "MESA_PANVK";
    case VK_DRIVER_ID_MESA_DOZEN:                 return "MESA_DOZEN";
    default:                                      return "UNKNOWN";
    }
}

static const char *device_type_str(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "DISCRETE_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "VIRTUAL_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
    default:                                     return "OTHER";
    }
}

static void emit_uuid(const char *k, const uint8_t *u, size_t n)
{
    char buf[64];
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < sizeof buf; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%02x", u[i]);
    buf[o] = 0;
    jstr(k, buf);
}

/* ------------------------------------------------------------------ */
/* Per-device probe                                                    */
/* ------------------------------------------------------------------ */

static void probe_device(VkInstance inst, VkPhysicalDevice pd, uint32_t index)
{
    (void)inst;

    /* --- device extensions first: everything below is gated on this --- */
    ExtList dx = { 0 };
    p_EnumerateDeviceExtensionProperties(pd, NULL, &dx.n, NULL);
    if (dx.n) {
        dx.e = calloc(dx.n, sizeof *dx.e);
        p_EnumerateDeviceExtensionProperties(pd, NULL, &dx.n, dx.e);
    }

    VkPhysicalDeviceProperties base_props;
    memset(&base_props, 0, sizeof base_props);
    p_GetPhysicalDeviceProperties(pd, &base_props);

    /* The device's own supported API version caps every promoted-core query. */
    uint32_t api = base_props.apiVersion;

    jobj(NULL);
    ju32("index", index);

    /* ---------------- identity ---------------- */
    jobj("device");
    jstr("deviceName", base_props.deviceName);
    jstr("deviceType", device_type_str(base_props.deviceType));
    ju32("vendorID", base_props.vendorID);
    ju32("deviceID", base_props.deviceID);
    emit_uuid("pipelineCacheUUID", base_props.pipelineCacheUUID, VK_UUID_SIZE);
    jobj_end();

    jobj("api");
    ju32("apiVersion_raw", api);
    {
        char v[32];
        snprintf(v, sizeof v, "%u.%u.%u", VK_API_VERSION_MAJOR(api),
                 VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
        jstr("apiVersion", v);
        snprintf(v, sizeof v, "%u.%u.%u", VK_API_VERSION_MAJOR(base_props.driverVersion),
                 VK_API_VERSION_MINOR(base_props.driverVersion),
                 VK_API_VERSION_PATCH(base_props.driverVersion));
        jstr("driverVersion_decoded", v);
    }
    ju32("driverVersion_raw", base_props.driverVersion);
    jbool("supports_1_1", api >= VK_API_VERSION_1_1);
    jbool("supports_1_2", api >= VK_API_VERSION_1_2);
    jbool("supports_1_3", api >= VK_API_VERSION_1_3);
#ifdef VK_API_VERSION_1_4
    jbool("supports_1_4", api >= VK_API_VERSION_1_4);
#else
    jstr("supports_1_4", "unknown-headers-too-old");
#endif
    jobj_end();

    /* ---------------- properties2 chain ---------------- */
    if (api < VK_API_VERSION_1_1 || !p_GetPhysicalDeviceProperties2) {
        jstr("__properties2", "not-available");
        jstr("__note", "device is Vulkan 1.0 only; feature/property structs cannot be queried");
        jarr("device_extensions");
        for (uint32_t i = 0; i < dx.n; i++) jelem_str(dx.e[i].extensionName);
        jarr_end();
        jobj_end();
        free(dx.e);
        return;
    }

    VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };

    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    const char *drv_src = src_of(api, VK_API_VERSION_1_2, &dx, "VK_KHR_driver_properties");
    if (avail(drv_src)) chain(&props2, &drv);

    VkPhysicalDeviceSubgroupProperties subgroup = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    const char *subgroup_src = src_of(api, VK_API_VERSION_1_1, &dx, NULL);
    if (avail(subgroup_src)) chain(&props2, &subgroup);

    VkPhysicalDeviceVulkan11Properties p11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
    if (api >= VK_API_VERSION_1_2) chain(&props2, &p11);

    VkPhysicalDeviceVulkan12Properties p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    if (api >= VK_API_VERSION_1_2) chain(&props2, &p12);

    VkPhysicalDeviceVulkan13Properties p13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    if (api >= VK_API_VERSION_1_3) chain(&props2, &p13);

    VkPhysicalDeviceMeshShaderPropertiesEXT meshp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT };
    const char *mesh_src = src_of(api, 0, &dx, "VK_EXT_mesh_shader");
    if (avail(mesh_src)) chain(&props2, &meshp);

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
    const char *rtp_src = src_of(api, 0, &dx, "VK_KHR_ray_tracing_pipeline");
    if (avail(rtp_src)) chain(&props2, &rtp);

    p_GetPhysicalDeviceProperties2(pd, &props2);

    /* ---------------- features2 chain ---------------- */
    VkPhysicalDeviceFeatures2 feat2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    if (api >= VK_API_VERSION_1_2) chain(&feat2, &f11);

    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    if (api >= VK_API_VERSION_1_2) chain(&feat2, &f12);

    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    if (api >= VK_API_VERSION_1_3) chain(&feat2, &f13);

    VkPhysicalDeviceMeshShaderFeaturesEXT meshf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    if (avail(mesh_src)) chain(&feat2, &meshf);

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    if (avail(rtp_src)) chain(&feat2, &rtf);

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    const char *acc_src = src_of(api, 0, &dx, "VK_KHR_acceleration_structure");
    if (avail(acc_src)) chain(&feat2, &accf);

    VkPhysicalDeviceRobustness2FeaturesEXT rob2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    const char *rob2_src = src_of(api, 0, &dx, "VK_EXT_robustness2");
    if (avail(rob2_src)) chain(&feat2, &rob2);

    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mut = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
    const char *mut_src = src_of(api, 0, &dx, "VK_EXT_mutable_descriptor_type");
    if (!avail(mut_src)) mut_src = src_of(api, 0, &dx, "VK_VALVE_mutable_descriptor_type");
    if (avail(mut_src)) chain(&feat2, &mut);

    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    const char *fsr_src = src_of(api, 0, &dx, "VK_KHR_fragment_shading_rate");
    if (avail(fsr_src)) chain(&feat2, &fsr);

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT };
    const char *atomf_src = src_of(api, 0, &dx, "VK_EXT_shader_atomic_float");
    if (avail(atomf_src)) chain(&feat2, &atomf);

    p_GetPhysicalDeviceFeatures2(pd, &feat2);

    /* ---------------- emit ---------------- */

    jobj("driver_properties");
    jstr("__source", drv_src);
    if (avail(drv_src)) {
        jstr("driverID", driver_id_str(drv.driverID));
        ju32("driverID_raw", drv.driverID);
        jstr("driverName", drv.driverName);
        jstr("driverInfo", drv.driverInfo);
        {
            char c[48];
            snprintf(c, sizeof c, "%u.%u.%u.%u", drv.conformanceVersion.major,
                     drv.conformanceVersion.minor, drv.conformanceVersion.subminor,
                     drv.conformanceVersion.patch);
            jstr("conformanceVersion", c);
        }
        jbool("is_turnip", drv.driverID == VK_DRIVER_ID_MESA_TURNIP);
        jbool("is_qualcomm_proprietary", drv.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY);
    }
    jobj_end();

    /*
     * A software rasterizer answering means we measured a CPU, not the GPU.
     * Every capability below would be that CPU renderer's, and reading them
     * as the device's would be worse than having no data at all -- so say so
     * loudly, in the JSON and on stderr.
     */
    {
        int sw = base_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                 (avail(drv_src) && (drv.driverID == VK_DRIVER_ID_MESA_LLVMPIPE ||
                                     drv.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER));
        jbool("is_software_rasterizer", sw);
        if (sw) {
            g_software_rasterizer = 1;
            jstr("__WARNING",
                 "SOFTWARE RASTERIZER -- this is a CPU renderer, not the device GPU. "
                 "Every capability in this file is the CPU renderer's answer and says "
                 "nothing about the real GPU. Re-run with "
                 "--lib /system/lib64/libvulkan.so to reach the vendor driver.");
            fprintf(stderr,
                "vkprobe: WARNING: device %u (%s) is a SOFTWARE RASTERIZER.\n"
                "  This is not the device GPU. Results are meaningless for hardware\n"
                "  capability. Re-run with: --lib /system/lib64/libvulkan.so\n",
                index, base_props.deviceName);
        }
    }

    /* base features (Vulkan 1.0 core) */
    jobj("features_core_1_0");
    const VkPhysicalDeviceFeatures *f = &feat2.features;
    jbool("robustBufferAccess", f->robustBufferAccess);
    jbool("fullDrawIndexUint32", f->fullDrawIndexUint32);
    jbool("imageCubeArray", f->imageCubeArray);
    jbool("independentBlend", f->independentBlend);
    jbool("geometryShader", f->geometryShader);
    jbool("tessellationShader", f->tessellationShader);
    jbool("sampleRateShading", f->sampleRateShading);
    jbool("dualSrcBlend", f->dualSrcBlend);
    jbool("logicOp", f->logicOp);
    jbool("multiDrawIndirect", f->multiDrawIndirect);
    jbool("drawIndirectFirstInstance", f->drawIndirectFirstInstance);
    jbool("depthClamp", f->depthClamp);
    jbool("depthBiasClamp", f->depthBiasClamp);
    jbool("fillModeNonSolid", f->fillModeNonSolid);
    jbool("depthBounds", f->depthBounds);
    jbool("samplerAnisotropy", f->samplerAnisotropy);
    jbool("textureCompressionETC2", f->textureCompressionETC2);
    jbool("textureCompressionASTC_LDR", f->textureCompressionASTC_LDR);
    jbool("textureCompressionBC", f->textureCompressionBC);
    jbool("occlusionQueryPrecise", f->occlusionQueryPrecise);
    jbool("pipelineStatisticsQuery", f->pipelineStatisticsQuery);
    jbool("fragmentStoresAndAtomics", f->fragmentStoresAndAtomics);
    jbool("vertexPipelineStoresAndAtomics", f->vertexPipelineStoresAndAtomics);
    jbool("shaderImageGatherExtended", f->shaderImageGatherExtended);
    jbool("shaderStorageImageExtendedFormats", f->shaderStorageImageExtendedFormats);
    jbool("shaderStorageImageMultisample", f->shaderStorageImageMultisample);
    jbool("shaderStorageImageReadWithoutFormat", f->shaderStorageImageReadWithoutFormat);
    jbool("shaderStorageImageWriteWithoutFormat", f->shaderStorageImageWriteWithoutFormat);
    jbool("shaderUniformBufferArrayDynamicIndexing", f->shaderUniformBufferArrayDynamicIndexing);
    jbool("shaderSampledImageArrayDynamicIndexing", f->shaderSampledImageArrayDynamicIndexing);
    jbool("shaderStorageBufferArrayDynamicIndexing", f->shaderStorageBufferArrayDynamicIndexing);
    jbool("shaderStorageImageArrayDynamicIndexing", f->shaderStorageImageArrayDynamicIndexing);
    jbool("shaderClipDistance", f->shaderClipDistance);
    jbool("shaderCullDistance", f->shaderCullDistance);
    jbool("shaderFloat64", f->shaderFloat64);
    jbool("shaderInt64", f->shaderInt64);
    jbool("shaderInt16", f->shaderInt16);
    jbool("shaderResourceResidency", f->shaderResourceResidency);
    jbool("shaderResourceMinLod", f->shaderResourceMinLod);
    jbool("sparseBinding", f->sparseBinding);
    jbool("sparseResidencyBuffer", f->sparseResidencyBuffer);
    jbool("sparseResidencyImage2D", f->sparseResidencyImage2D);
    jbool("sparseResidencyImage3D", f->sparseResidencyImage3D);
    jbool("sparseResidencyAliased", f->sparseResidencyAliased);
    jbool("variableMultisampleRate", f->variableMultisampleRate);
    jbool("inheritedQueries", f->inheritedQueries);
    jobj_end();

    jobj("features_1_1");
    jstr("__source", api >= VK_API_VERSION_1_2 ? "core1.2-struct" : "not-available");
    if (api >= VK_API_VERSION_1_2) {
        jbool("storageBuffer16BitAccess", f11.storageBuffer16BitAccess);
        jbool("uniformAndStorageBuffer16BitAccess", f11.uniformAndStorageBuffer16BitAccess);
        jbool("multiview", f11.multiview);
        jbool("variablePointers", f11.variablePointers);
        jbool("shaderDrawParameters", f11.shaderDrawParameters);
        jbool("samplerYcbcrConversion", f11.samplerYcbcrConversion);
    }
    jobj_end();

    jobj("features_1_2");
    jstr("__source", api >= VK_API_VERSION_1_2 ? "core1.2" : "not-available");
    if (api >= VK_API_VERSION_1_2) {
        /* descriptor indexing -- required by VKD3D-Proton for the D3D12 heap model */
        jbool("descriptorIndexing", f12.descriptorIndexing);
        jbool("shaderSampledImageArrayNonUniformIndexing", f12.shaderSampledImageArrayNonUniformIndexing);
        jbool("shaderStorageBufferArrayNonUniformIndexing", f12.shaderStorageBufferArrayNonUniformIndexing);
        jbool("shaderStorageImageArrayNonUniformIndexing", f12.shaderStorageImageArrayNonUniformIndexing);
        jbool("descriptorBindingPartiallyBound", f12.descriptorBindingPartiallyBound);
        jbool("descriptorBindingVariableDescriptorCount", f12.descriptorBindingVariableDescriptorCount);
        jbool("descriptorBindingSampledImageUpdateAfterBind", f12.descriptorBindingSampledImageUpdateAfterBind);
        jbool("descriptorBindingStorageBufferUpdateAfterBind", f12.descriptorBindingStorageBufferUpdateAfterBind);
        jbool("descriptorBindingUpdateUnusedWhilePending", f12.descriptorBindingUpdateUnusedWhilePending);
        jbool("runtimeDescriptorArray", f12.runtimeDescriptorArray);
        /* timeline semaphore -- required by VKD3D-Proton for ID3D12Fence */
        jbool("timelineSemaphore", f12.timelineSemaphore);
        /* buffer device address -- required for D3D12 root descriptors */
        jbool("bufferDeviceAddress", f12.bufferDeviceAddress);
        jbool("bufferDeviceAddressCaptureReplay", f12.bufferDeviceAddressCaptureReplay);
        /* fp16 / int8 -- shader perf on mobile */
        jbool("shaderFloat16", f12.shaderFloat16);
        jbool("shaderInt8", f12.shaderInt8);
        jbool("storageBuffer8BitAccess", f12.storageBuffer8BitAccess);
        jbool("hostQueryReset", f12.hostQueryReset);
        jbool("scalarBlockLayout", f12.scalarBlockLayout);
        jbool("separateDepthStencilLayouts", f12.separateDepthStencilLayouts);
        jbool("imagelessFramebuffer", f12.imagelessFramebuffer);
        jbool("samplerFilterMinmax", f12.samplerFilterMinmax);
        jbool("drawIndirectCount", f12.drawIndirectCount);
        jbool("shaderOutputViewportIndex", f12.shaderOutputViewportIndex);
        jbool("shaderOutputLayer", f12.shaderOutputLayer);
        jbool("vulkanMemoryModel", f12.vulkanMemoryModel);
    }
    jobj_end();

    jobj("features_1_3");
    jstr("__source", api >= VK_API_VERSION_1_3 ? "core1.3" : "not-available");
    if (api >= VK_API_VERSION_1_3) {
        jbool("synchronization2", f13.synchronization2);
        jbool("dynamicRendering", f13.dynamicRendering);
        jbool("maintenance4", f13.maintenance4);
        jbool("subgroupSizeControl", f13.subgroupSizeControl);
        jbool("computeFullSubgroups", f13.computeFullSubgroups);
        jbool("shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation);
        jbool("shaderTerminateInvocation", f13.shaderTerminateInvocation);
        jbool("shaderZeroInitializeWorkgroupMemory", f13.shaderZeroInitializeWorkgroupMemory);
        jbool("pipelineCreationCacheControl", f13.pipelineCreationCacheControl);
        jbool("privateData", f13.privateData);
        jbool("inlineUniformBlock", f13.inlineUniformBlock);
        jbool("textureCompressionASTC_HDR", f13.textureCompressionASTC_HDR);
        jbool("shaderIntegerDotProduct", f13.shaderIntegerDotProduct);
    }
    jobj_end();

    /* 1.3-and-below fallbacks: if the device is not 1.2/1.3, these caps may
     * still be present as extensions. Report the extension presence so the
     * answer is never silently "false" when it is really "via extension". */
    jobj("promoted_as_extension");
    jstr("__note", "extension-level availability of caps that are core in 1.2/1.3");
    jbool("VK_EXT_descriptor_indexing", ext_has(&dx, "VK_EXT_descriptor_indexing"));
    jbool("VK_KHR_timeline_semaphore", ext_has(&dx, "VK_KHR_timeline_semaphore"));
    jbool("VK_KHR_buffer_device_address", ext_has(&dx, "VK_KHR_buffer_device_address"));
    jbool("VK_KHR_synchronization2", ext_has(&dx, "VK_KHR_synchronization2"));
    jbool("VK_KHR_dynamic_rendering", ext_has(&dx, "VK_KHR_dynamic_rendering"));
    jbool("VK_KHR_shader_float16_int8", ext_has(&dx, "VK_KHR_shader_float16_int8"));
    jbool("VK_EXT_shader_demote_to_helper_invocation", ext_has(&dx, "VK_EXT_shader_demote_to_helper_invocation"));
    jbool("VK_EXT_pipeline_creation_cache_control", ext_has(&dx, "VK_EXT_pipeline_creation_cache_control"));
    jobj_end();

    jobj("subgroup");
    jstr("__source", subgroup_src);
    if (avail(subgroup_src)) {
        ju32("subgroupSize", subgroup.subgroupSize);
        jarr("supportedStages");
        if (subgroup.supportedStages & VK_SHADER_STAGE_VERTEX_BIT)   jelem_str("VERTEX");
        if (subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) jelem_str("FRAGMENT");
        if (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)  jelem_str("COMPUTE");
        jarr_end();
        jarr("supportedOperations");
        VkSubgroupFeatureFlags o = subgroup.supportedOperations;
        if (o & VK_SUBGROUP_FEATURE_BASIC_BIT)            jelem_str("BASIC");
        if (o & VK_SUBGROUP_FEATURE_VOTE_BIT)             jelem_str("VOTE");
        if (o & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)       jelem_str("ARITHMETIC");
        if (o & VK_SUBGROUP_FEATURE_BALLOT_BIT)           jelem_str("BALLOT");
        if (o & VK_SUBGROUP_FEATURE_SHUFFLE_BIT)          jelem_str("SHUFFLE");
        if (o & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) jelem_str("SHUFFLE_RELATIVE");
        if (o & VK_SUBGROUP_FEATURE_CLUSTERED_BIT)        jelem_str("CLUSTERED");
        if (o & VK_SUBGROUP_FEATURE_QUAD_BIT)             jelem_str("QUAD");
        jarr_end();
        jbool("quadOperationsInAllStages", subgroup.quadOperationsInAllStages);
    }
    if (api >= VK_API_VERSION_1_3) {
        ju32("minSubgroupSize", p13.minSubgroupSize);
        ju32("maxSubgroupSize", p13.maxSubgroupSize);
    }
    jobj_end();

    jobj("mesh_shader");
    jstr("__source", mesh_src);
    if (avail(mesh_src)) {
        jbool("taskShader", meshf.taskShader);
        jbool("meshShader", meshf.meshShader);
        jbool("multiviewMeshShader", meshf.multiviewMeshShader);
        ju32("maxMeshWorkGroupInvocations", meshp.maxMeshWorkGroupInvocations);
        ju32("maxTaskWorkGroupInvocations", meshp.maxTaskWorkGroupInvocations);
    }
    jobj_end();

    jobj("ray_tracing");
    jstr("__pipeline_source", rtp_src);
    jstr("__accel_source", acc_src);
    if (avail(rtp_src)) {
        jbool("rayTracingPipeline", rtf.rayTracingPipeline);
        ju32("maxRayRecursionDepth", rtp.maxRayRecursionDepth);
        ju32("shaderGroupHandleSize", rtp.shaderGroupHandleSize);
    }
    if (avail(acc_src)) {
        jbool("accelerationStructure", accf.accelerationStructure);
        jbool("accelerationStructureHostCommands", accf.accelerationStructureHostCommands);
    }
    jbool("VK_KHR_ray_query", ext_has(&dx, "VK_KHR_ray_query"));
    jobj_end();

    jobj("robustness2");
    jstr("__source", rob2_src);
    if (avail(rob2_src)) {
        jbool("robustBufferAccess2", rob2.robustBufferAccess2);
        jbool("robustImageAccess2", rob2.robustImageAccess2);
        /* nullDescriptor is what D3D12 null-descriptor semantics map onto */
        jbool("nullDescriptor", rob2.nullDescriptor);
    }
    jobj_end();

    jobj("mutable_descriptor_type");
    jstr("__source", mut_src);
    if (avail(mut_src))
        jbool("mutableDescriptorType", mut.mutableDescriptorType);
    jobj_end();

    jobj("fragment_shading_rate");
    jstr("__source", fsr_src);
    if (avail(fsr_src)) {
        jbool("pipelineFragmentShadingRate", fsr.pipelineFragmentShadingRate);
        jbool("primitiveFragmentShadingRate", fsr.primitiveFragmentShadingRate);
        jbool("attachmentFragmentShadingRate", fsr.attachmentFragmentShadingRate);
    }
    jobj_end();

    jobj("shader_atomic_float");
    jstr("__source", atomf_src);
    if (avail(atomf_src)) {
        jbool("shaderBufferFloat32Atomics", atomf.shaderBufferFloat32Atomics);
        jbool("shaderBufferFloat32AtomicAdd", atomf.shaderBufferFloat32AtomicAdd);
        jbool("shaderSharedFloat32Atomics", atomf.shaderSharedFloat32Atomics);
        jbool("shaderImageFloat32Atomics", atomf.shaderImageFloat32Atomics);
    }
    jstr("VK_KHR_shader_atomic_int64", ext_has(&dx, "VK_KHR_shader_atomic_int64") ? "present" : "absent");
    jobj_end();

    /* external memory / semaphore -- Wine, Android AHardwareBuffer, swapchain sync */
    jobj("external");
    jbool("VK_KHR_external_memory_fd", ext_has(&dx, "VK_KHR_external_memory_fd"));
    jbool("VK_KHR_external_semaphore_fd", ext_has(&dx, "VK_KHR_external_semaphore_fd"));
    jbool("VK_KHR_external_fence_fd", ext_has(&dx, "VK_KHR_external_fence_fd"));
    jbool("VK_ANDROID_external_memory_android_hardware_buffer",
          ext_has(&dx, "VK_ANDROID_external_memory_android_hardware_buffer"));
    jbool("VK_EXT_external_memory_dma_buf", ext_has(&dx, "VK_EXT_external_memory_dma_buf"));

    if (p_GetPhysicalDeviceExternalSemaphoreProperties && api >= VK_API_VERSION_1_1) {
        static const struct { VkExternalSemaphoreHandleTypeFlagBits t; const char *n; } sem[] = {
            { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
            { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,   "SYNC_FD"   },
        };
        jobj("semaphore_handle_types");
        for (size_t i = 0; i < sizeof sem / sizeof *sem; i++) {
            VkPhysicalDeviceExternalSemaphoreInfo in = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO
            };
            in.handleType = sem[i].t;
            VkExternalSemaphoreProperties out = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES
            };
            p_GetPhysicalDeviceExternalSemaphoreProperties(pd, &in, &out);
            jobj(sem[i].n);
            jbool("importable", (out.externalSemaphoreFeatures &
                                 VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0);
            jbool("exportable", (out.externalSemaphoreFeatures &
                                 VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0);
            jobj_end();
        }
        jobj_end();
    }
    jobj_end();

    jobj("pipeline_cache");
    emit_uuid("pipelineCacheUUID", base_props.pipelineCacheUUID, VK_UUID_SIZE);
    jstr("__note", "pipelineCacheUUID must be part of the Star Bionic cache key; "
                   "a mismatch means the cache blob is not reusable");
    if (api >= VK_API_VERSION_1_3)
        jbool("pipelineCreationCacheControl", f13.pipelineCreationCacheControl);
    jobj_end();

    /* sparse -- D3D12 reserved/tiled resources map onto these */
    jobj("sparse_properties");
    jbool("residencyStandard2DBlockShape", base_props.sparseProperties.residencyStandard2DBlockShape);
    jbool("residencyStandard2DMultisampleBlockShape", base_props.sparseProperties.residencyStandard2DMultisampleBlockShape);
    jbool("residencyStandard3DBlockShape", base_props.sparseProperties.residencyStandard3DBlockShape);
    jbool("residencyAlignedMipSize", base_props.sparseProperties.residencyAlignedMipSize);
    jbool("residencyNonResidentStrict", base_props.sparseProperties.residencyNonResidentStrict);
    jobj_end();

    /* selected limits that gate D3D12 emulation */
    jobj("limits");
    const VkPhysicalDeviceLimits *L = &base_props.limits;
    ju32("maxImageDimension2D", L->maxImageDimension2D);
    ju32("maxBoundDescriptorSets", L->maxBoundDescriptorSets);
    ju32("maxPerStageDescriptorSampledImages", L->maxPerStageDescriptorSampledImages);
    ju32("maxPerStageDescriptorStorageBuffers", L->maxPerStageDescriptorStorageBuffers);
    ju32("maxPerStageResources", L->maxPerStageResources);
    ju32("maxPushConstantsSize", L->maxPushConstantsSize);
    ju32("maxComputeWorkGroupInvocations", L->maxComputeWorkGroupInvocations);
    ju32("maxVertexInputAttributes", L->maxVertexInputAttributes);
    ju32("maxColorAttachments", L->maxColorAttachments);
    ju64("maxMemoryAllocationCount", L->maxMemoryAllocationCount);
    ju64("bufferImageGranularity", (uint64_t)L->bufferImageGranularity);
    ju64("minUniformBufferOffsetAlignment", (uint64_t)L->minUniformBufferOffsetAlignment);
    ju64("minStorageBufferOffsetAlignment", (uint64_t)L->minStorageBufferOffsetAlignment);
    ju64("nonCoherentAtomSize", (uint64_t)L->nonCoherentAtomSize);
    jf32("timestampPeriod", L->timestampPeriod);
    jbool("timestampComputeAndGraphics", L->timestampComputeAndGraphics);
    jf32("maxSamplerAnisotropy", L->maxSamplerAnisotropy);
    if (api >= VK_API_VERSION_1_2)
        ju32("maxUpdateAfterBindDescriptorsInAllPools", p12.maxUpdateAfterBindDescriptorsInAllPools);
    jobj_end();

    /* memory + budget */
    {
        VkPhysicalDeviceMemoryProperties2 mp2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT
        };
        const char *budget_src = src_of(api, 0, &dx, "VK_EXT_memory_budget");
        if (avail(budget_src)) chain(&mp2, &budget);
        if (p_GetPhysicalDeviceMemoryProperties2)
            p_GetPhysicalDeviceMemoryProperties2(pd, &mp2);

        jobj("memory");
        jstr("__budget_source", budget_src);
        jarr("heaps");
        for (uint32_t i = 0; i < mp2.memoryProperties.memoryHeapCount; i++) {
            jobj(NULL);
            ju32("index", i);
            ju64("size", mp2.memoryProperties.memoryHeaps[i].size);
            jbool("device_local", (mp2.memoryProperties.memoryHeaps[i].flags &
                                   VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0);
            if (avail(budget_src)) {
                ju64("budget", budget.heapBudget[i]);
                ju64("usage", budget.heapUsage[i]);
            }
            jobj_end();
        }
        jarr_end();
        jarr("types");
        for (uint32_t i = 0; i < mp2.memoryProperties.memoryTypeCount; i++) {
            VkMemoryPropertyFlags pf = mp2.memoryProperties.memoryTypes[i].propertyFlags;
            jobj(NULL);
            ju32("index", i);
            ju32("heap", mp2.memoryProperties.memoryTypes[i].heapIndex);
            jarr("flags");
            if (pf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)     jelem_str("DEVICE_LOCAL");
            if (pf & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)     jelem_str("HOST_VISIBLE");
            if (pf & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)    jelem_str("HOST_COHERENT");
            if (pf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)      jelem_str("HOST_CACHED");
            if (pf & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) jelem_str("LAZILY_ALLOCATED");
            jarr_end();
            jobj_end();
        }
        jarr_end();
        jobj_end();
    }

    /* queues -- VKD3D-Proton maps D3D12 direct/compute/copy queues onto these */
    {
        uint32_t qn = 0;
        p_GetPhysicalDeviceQueueFamilyProperties2(pd, &qn, NULL);
        VkQueueFamilyProperties2 *qf = calloc(qn ? qn : 1, sizeof *qf);
        for (uint32_t i = 0; i < qn; i++)
            qf[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        p_GetPhysicalDeviceQueueFamilyProperties2(pd, &qn, qf);

        jarr("queue_families");
        for (uint32_t i = 0; i < qn; i++) {
            VkQueueFlags fl = qf[i].queueFamilyProperties.queueFlags;
            jobj(NULL);
            ju32("index", i);
            ju32("queueCount", qf[i].queueFamilyProperties.queueCount);
            ju32("timestampValidBits", qf[i].queueFamilyProperties.timestampValidBits);
            jarr("flags");
            if (fl & VK_QUEUE_GRAPHICS_BIT)       jelem_str("GRAPHICS");
            if (fl & VK_QUEUE_COMPUTE_BIT)        jelem_str("COMPUTE");
            if (fl & VK_QUEUE_TRANSFER_BIT)       jelem_str("TRANSFER");
            if (fl & VK_QUEUE_SPARSE_BINDING_BIT) jelem_str("SPARSE_BINDING");
            jarr_end();
            jobj_end();
        }
        jarr_end();
        free(qf);
    }

    /* formats */
    {
        int have_fp2 = p_GetPhysicalDeviceFormatProperties2 != NULL;
        int have_fp3 = ext_has(&dx, "VK_KHR_format_feature_flags2") ||
                       api >= VK_API_VERSION_1_3;
        jobj("formats");
        jstr("__source", have_fp2 ? (have_fp3 ? "formatProperties3" : "formatProperties2")
                                  : "not-available");
        for (size_t i = 0; have_fp2 && i < sizeof g_formats / sizeof *g_formats; i++) {
            VkFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
            VkFormatProperties3 fp3 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
            if (have_fp3) chain(&fp, &fp3);
            p_GetPhysicalDeviceFormatProperties2(pd, g_formats[i].fmt, &fp);

            VkFormatFeatureFlags2 opt = have_fp3 ? fp3.optimalTilingFeatures
                                                 : (VkFormatFeatureFlags2)fp.formatProperties.optimalTilingFeatures;
            VkFormatFeatureFlags2 buf = have_fp3 ? fp3.bufferFeatures
                                                 : (VkFormatFeatureFlags2)fp.formatProperties.bufferFeatures;

            jobj(g_formats[i].name);
            jstr("group", g_formats[i].group);
            jbool("supported", (opt | buf) != 0);
            emit_format_flags("optimalTiling", opt);
            emit_format_flags("buffer", buf);
            jobj_end();
        }
        jobj_end();
    }

    jarr("device_extensions");
    for (uint32_t i = 0; i < dx.n; i++) jelem_str(dx.e[i].extensionName);
    jarr_end();
    ju32("device_extension_count", dx.n);

    /*
     * D3D12-relevant checklist. INFORMATIONAL ONLY: this is a report of caps
     * that a D3D12 translation layer generally leans on. The authoritative
     * requirement list is whatever the VKD3D-Proton build we pin in Stage 4
     * checks at runtime -- do not treat an all-true result here as "FH6 will
     * run", and do not treat a false as "impossible" without confirming
     * against that build's own device-capability check.
     */
    jobj("d3d12_relevant");
    jstr("__disclaimer",
         "informational; authoritative requirements come from the pinned "
         "VKD3D-Proton build, not from this list");
    jbool("timelineSemaphore", api >= VK_API_VERSION_1_2 ? f12.timelineSemaphore
                                : (VkBool32)ext_has(&dx, "VK_KHR_timeline_semaphore"));
    jbool("descriptorIndexing", api >= VK_API_VERSION_1_2 ? f12.descriptorIndexing
                                : (VkBool32)ext_has(&dx, "VK_EXT_descriptor_indexing"));
    jbool("bufferDeviceAddress", api >= VK_API_VERSION_1_2 ? f12.bufferDeviceAddress
                                : (VkBool32)ext_has(&dx, "VK_KHR_buffer_device_address"));
    jbool("nullDescriptor", avail(rob2_src) ? rob2.nullDescriptor : VK_FALSE);
    jbool("mutableDescriptorType", avail(mut_src) ? mut.mutableDescriptorType : VK_FALSE);
    jbool("textureCompressionBC", f->textureCompressionBC);
    jbool("shaderInt64", f->shaderInt64);
    jbool("sparseBinding", f->sparseBinding);
    jbool("VK_KHR_push_descriptor", ext_has(&dx, "VK_KHR_push_descriptor"));
    ju32("VK_KHR_push_descriptor_rev", ext_rev(&dx, "VK_KHR_push_descriptor"));
    jobj_end();

    jobj_end();
    free(dx.e);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fputs(
      "vkprobe " PROBE_VERSION " -- Star Bionic Vulkan capability probe\n"
      "\n"
      "  --lib PATH     dlopen this Vulkan loader instead of the default\n"
      "  --icd PATH     set VK_ICD_FILENAMES=PATH before loading (pick Turnip\n"
      "                 vs. the vendor blob explicitly)\n"
      "  --device N     probe only physical device N\n"
      "  --out FILE     write JSON to FILE instead of stdout. Use this inside\n"
      "                 Winlator, whose launcher gives you no shell to redirect\n"
      "                 with.\n"
      "  -h, --help     this text\n"
      "\n"
      "Writes a JSON report to stdout. Diagnostics go to stderr, so\n"
      "`vkprobe > caps.json` always yields valid JSON.\n"
      "\n"
      "NOTE: a standalone binary uses the loader/ICD visible to ITS process.\n"
      "An AdrenoTools-style custom driver injected into another app is NOT\n"
      "picked up here -- point --icd at the same driver, or run this inside\n"
      "the same container, or you are measuring the wrong driver.\n",
      stderr);
}

int main(int argc, char **argv)
{
    const char *libpath = NULL;
    const char *outpath = NULL;
    long only_device = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lib") && i + 1 < argc) {
            libpath = argv[++i];
        } else if (!strcmp(argv[i], "--icd") && i + 1 < argc) {
            SETENV("VK_ICD_FILENAMES", argv[++i]);
            SETENV("VK_DRIVER_FILES", argv[i]);
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            outpath = argv[++i];
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            only_device = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "vkprobe: unknown argument '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    /* Every emitter writes to stdout, so redirecting it here covers all of
     * them. Done before any probing so a failure report lands in the file
     * too -- a run that fails inside Winlator still has to be diagnosable. */
    if (outpath && !freopen(outpath, "w", stdout)) {
        fprintf(stderr, "vkprobe: cannot open --out file '%s'\n", outpath);
        return 1;
    }

    if (!load_loader(libpath))
        return 1;

    uint32_t inst_api = VK_API_VERSION_1_0;
    if (p_EnumerateInstanceVersion)
        p_EnumerateInstanceVersion(&inst_api);

    jneed[0] = 0;
    jobj(NULL);
    jstr("probe_version", PROBE_VERSION);
    jstr("loader_path", g_libpath ? g_libpath : "(default)");
    {
        const char *icd = getenv("VK_ICD_FILENAMES");
        if (icd) jstr("VK_ICD_FILENAMES", icd); else jnull("VK_ICD_FILENAMES");
    }
    {
        char v[32];
        snprintf(v, sizeof v, "%u.%u.%u", VK_API_VERSION_MAJOR(inst_api),
                 VK_API_VERSION_MINOR(inst_api), VK_API_VERSION_PATCH(inst_api));
        jstr("instance_api_version", v);
    }
    {
        char v[32];
        snprintf(v, sizeof v, "%u.%u.%u", VK_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),
                 VK_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE),
                 VK_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));
        jstr("built_against_headers", v);
    }

    /* instance extensions + layers */
    {
        ExtList ie = { 0 };
        if (p_EnumerateInstanceExtensionProperties) {
            p_EnumerateInstanceExtensionProperties(NULL, &ie.n, NULL);
            if (ie.n) {
                ie.e = calloc(ie.n, sizeof *ie.e);
                p_EnumerateInstanceExtensionProperties(NULL, &ie.n, ie.e);
            }
        }
        jarr("instance_extensions");
        for (uint32_t i = 0; i < ie.n; i++) jelem_str(ie.e[i].extensionName);
        jarr_end();
        free(ie.e);

        uint32_t ln = 0;
        if (p_EnumerateInstanceLayerProperties)
            p_EnumerateInstanceLayerProperties(&ln, NULL);
        VkLayerProperties *lp = ln ? calloc(ln, sizeof *lp) : NULL;
        if (lp) p_EnumerateInstanceLayerProperties(&ln, lp);
        jarr("instance_layers");
        for (uint32_t i = 0; i < ln; i++) jelem_str(lp[i].layerName);
        jarr_end();
        free(lp);
    }

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "star-bionic-vkprobe";
    ai.apiVersion = inst_api;

    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = p_CreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        jsep(); jesc("error"); fputs(": ", stdout);
        { char e[64]; snprintf(e, sizeof e, "vkCreateInstance failed: %d", r); jesc(e); }
        jobj_end();
        fputc('\n', stdout);
        fprintf(stderr, "vkprobe: vkCreateInstance failed (%d)\n", r);
        return 1;
    }

    p_DestroyInstance                = (PFN_vkDestroyInstance)GIPA(inst, "vkDestroyInstance");
    p_EnumeratePhysicalDevices       = (PFN_vkEnumeratePhysicalDevices)GIPA(inst, "vkEnumeratePhysicalDevices");
    p_EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)GIPA(inst, "vkEnumerateDeviceExtensionProperties");
    p_GetPhysicalDeviceProperties    = (PFN_vkGetPhysicalDeviceProperties)GIPA(inst, "vkGetPhysicalDeviceProperties");
    p_GetPhysicalDeviceProperties2   = (PFN_vkGetPhysicalDeviceProperties2)GIPA(inst, "vkGetPhysicalDeviceProperties2");
    p_GetPhysicalDeviceFeatures2     = (PFN_vkGetPhysicalDeviceFeatures2)GIPA(inst, "vkGetPhysicalDeviceFeatures2");
    p_GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)GIPA(inst, "vkGetPhysicalDeviceMemoryProperties2");
    p_GetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2)GIPA(inst, "vkGetPhysicalDeviceFormatProperties2");
    p_GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)GIPA(inst, "vkGetPhysicalDeviceQueueFamilyProperties2");
    p_GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)GIPA(inst, "vkGetPhysicalDeviceExternalSemaphoreProperties");

    /* KHR fallbacks for a 1.0 instance that has the extensions */
    if (!p_GetPhysicalDeviceProperties2)
        p_GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)GIPA(inst, "vkGetPhysicalDeviceProperties2KHR");
    if (!p_GetPhysicalDeviceFeatures2)
        p_GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)GIPA(inst, "vkGetPhysicalDeviceFeatures2KHR");

    uint32_t n = 0;
    p_EnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *pds = n ? calloc(n, sizeof *pds) : NULL;
    if (pds) p_EnumeratePhysicalDevices(inst, &n, pds);

    ju32("physical_device_count", n);
    jarr("physical_devices");
    for (uint32_t i = 0; i < n; i++) {
        if (only_device >= 0 && (uint32_t)only_device != i)
            continue;
        probe_device(inst, pds[i], i);
    }
    jarr_end();

    jobj_end();
    fputc('\n', stdout);

    free(pds);
    if (p_DestroyInstance) p_DestroyInstance(inst, NULL);

    if (g_software_rasterizer)
        fprintf(stderr,
            "\nvkprobe: this capture is NOT the device GPU. Do not use it for\n"
            "         hardware capability decisions.\n");

    return 0;
}

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# star-bionic report -- compact, paste-able Stage 2 summary from a vkprobe
# capture. Needs nothing but grep/sed/awk, so it runs anywhere the probe does.
#
#   ./tools/report.sh collect-*/caps-android.json
#
# Reads only what the probe recorded. Any field the probe did not capture
# prints as "?" rather than being guessed at.
set -uo pipefail

F="${1:-}"
if [[ -z "$F" || ! -r "$F" ]]; then
  # Convenience: pick the newest android capture if none named.
  F=$(ls -t collect-*/caps-android.json 2>/dev/null | head -1)
fi
[[ -n "$F" && -r "$F" ]] || { echo "usage: report.sh <caps.json>" >&2; exit 2; }

# first scalar value for a key
g()  { grep -o "\"$1\": [a-z0-9]*" "$F" | head -1 | awk '{print $2}'; }
# first string value for a key
gs() { grep -o "\"$1\": \"[^\"]*\"" "$F" | head -1 | cut -d'"' -f4; }
# "supported" inside a named format block
fm() { grep -A3 "\"$1\": {" "$F" | grep -o '"supported": [a-z]*' | head -1 | awk '{print $2}'; }

y() { case "${1:-}" in true) printf 'YES';; false) printf 'no ';; *) printf ' ? ';; esac; }

row() { printf '  %-38s %s\n' "$1" "$(y "$2")"; }

echo "================ STAR BIONIC / STAGE 2 ================"
echo "source: $F"
echo
echo "--- DRIVER ---"
printf '  %-38s %s\n' "driverName"   "$(gs driverName)"
printf '  %-38s %s\n' "driverID"     "$(gs driverID)"
printf '  %-38s %s\n' "driverInfo"   "$(gs driverInfo)"
printf '  %-38s %s\n' "conformance"  "$(gs conformanceVersion)"
printf '  %-38s %s\n' "deviceName"   "$(gs deviceName)"
row "is_software_rasterizer (must be no)" "$(g is_software_rasterizer)"
echo
echo "--- API ---"
printf '  %-38s %s\n' "apiVersion" "$(gs apiVersion)"
row "supports_1_1" "$(g supports_1_1)"
row "supports_1_2" "$(g supports_1_2)"
row "supports_1_3" "$(g supports_1_3)"
echo
echo "--- VKD3D-PROTON / DX12 CRITICAL ---"
row "textureCompressionBC  (FH6 textures)" "$(g textureCompressionBC)"
row "descriptorIndexing"                   "$(g descriptorIndexing)"
row "runtimeDescriptorArray"               "$(g runtimeDescriptorArray)"
row "descriptorBindingPartiallyBound"      "$(g descriptorBindingPartiallyBound)"
row "timelineSemaphore     (ID3D12Fence)"  "$(g timelineSemaphore)"
row "bufferDeviceAddress"                  "$(g bufferDeviceAddress)"
row "nullDescriptor        (robustness2)"  "$(g nullDescriptor)"
row "mutableDescriptorType"                "$(g mutableDescriptorType)"
row "synchronization2"                     "$(g synchronization2)"
row "dynamicRendering"                     "$(g dynamicRendering)"
row "shaderInt64"                          "$(g shaderInt64)"
row "shaderDemoteToHelperInvocation"       "$(g shaderDemoteToHelperInvocation)"
row "pipelineCreationCacheControl"         "$(g pipelineCreationCacheControl)"
printf '  %-38s %s\n' "VK_KHR_push_descriptor" "$(y "$(g VK_KHR_push_descriptor)")"
echo
echo "--- SHADER / PERF ---"
row "shaderFloat16 (fp16)"                 "$(g shaderFloat16)"
row "shaderInt8"                           "$(g shaderInt8)"
row "shaderStorageImageReadWithoutFormat"  "$(g shaderStorageImageReadWithoutFormat)"
row "fragmentStoresAndAtomics"             "$(g fragmentStoresAndAtomics)"
row "samplerAnisotropy"                    "$(g samplerAnisotropy)"
row "depthBounds"                          "$(g depthBounds)"
row "geometryShader"                       "$(g geometryShader)"
row "tessellationShader"                   "$(g tessellationShader)"
printf '  %-38s %s\n' "subgroupSize"    "$(g subgroupSize)"
printf '  %-38s %s / %s\n' "min/maxSubgroupSize" "$(g minSubgroupSize)" "$(g maxSubgroupSize)"
echo
echo "--- TEXTURE FORMATS ---"
for f in BC1_RGBA_UNORM BC3_UNORM BC4_UNORM BC5_UNORM BC6H_UFLOAT BC7_UNORM BC7_SRGB \
         ASTC_4x4_UNORM ASTC_8x8_UNORM ETC2_R8G8B8A8_UNORM; do
  row "$f" "$(fm "$f")"
done
echo
echo "--- DEPTH / RENDER TARGET FORMATS ---"
for f in D16_UNORM D24_UNORM_S8_UINT D32_SFLOAT D32_SFLOAT_S8_UINT S8_UINT \
         R16G16B16A16_SFLOAT B10G11R11_UFLOAT A2B10G10R10_UNORM; do
  row "$f" "$(fm "$f")"
done
echo
echo "--- SPARSE (D3D12 reserved resources) ---"
row "sparseBinding"           "$(g sparseBinding)"
row "sparseResidencyBuffer"   "$(g sparseResidencyBuffer)"
row "sparseResidencyImage2D"  "$(g sparseResidencyImage2D)"
echo
echo "--- ADVANCED ---"
printf '  %-38s %s\n' "mesh_shader __source"  "$(grep -A1 '"mesh_shader"' "$F" | grep -o '"__source": "[^"]*"' | cut -d'"' -f4)"
row "meshShader"           "$(g meshShader)"
printf '  %-38s %s\n' "ray_tracing __pipeline_source" "$(grep -A1 '"ray_tracing"' "$F" | grep -o '"__pipeline_source": "[^"]*"' | cut -d'"' -f4)"
row "rayTracingPipeline"   "$(g rayTracingPipeline)"
row "VK_KHR_ray_query"     "$(g VK_KHR_ray_query)"
row "attachmentFragmentShadingRate" "$(g attachmentFragmentShadingRate)"
echo
echo "--- MEMORY ---"
printf '  %-38s %s\n' "memory_budget __source" "$(gs __budget_source)"
if command -v python3 >/dev/null 2>&1; then
python3 - "$F" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
dev=d["physical_devices"][0]
for h in dev["memory"]["heaps"]:
    extra=""
    if "budget" in h:
        extra=f"  budget={h['budget']/2**30:.2f} GiB  usage={h['usage']/2**30:.2f} GiB"
    print(f"  heap{h['index']}  {h['size']/2**30:7.2f} GiB  device_local={h['device_local']}{extra}")
print(f"  memory types: {len(dev['memory']['types'])}")
PY
else
  echo "  (pkg install python for heap detail; raw sizes below)"
  grep -o '"size": [0-9]*' "$F" | head -4 | awk '{printf "  heap size: %.2f GiB\n", $2/1073741824}'
fi
echo
echo "--- LIMITS ---"
for k in maxImageDimension2D maxBoundDescriptorSets maxPerStageResources \
         maxPushConstantsSize maxComputeWorkGroupInvocations maxColorAttachments \
         maxMemoryAllocationCount maxUpdateAfterBindDescriptorsInAllPools; do
  printf '  %-38s %s\n' "$k" "$(g $k)"
done
printf '  %-38s %s\n' "timestampPeriod (ns)" "$(grep -o '"timestampPeriod": [0-9.e+-]*' "$F" | head -1 | awk '{print $2}')"
row "timestampComputeAndGraphics" "$(g timestampComputeAndGraphics)"
echo
echo "--- QUEUES ---"
grep -o '"queueCount": [0-9]*' "$F" | awk '{printf "  queue family: count=%s\n", $2}'
echo
printf '  %-38s %s\n' "device_extension_count" "$(g device_extension_count)"
printf '  %-38s %s\n' "pipelineCacheUUID" "$(gs pipelineCacheUUID)"
echo "======================================================"

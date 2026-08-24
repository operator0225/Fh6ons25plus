#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render a vkbench result as a compact, paste-able summary."""
import json, sys, glob, os

path = sys.argv[1] if len(sys.argv) > 1 else None
if not path:
    c = sorted(glob.glob("/sdcard/Download/vkbench-result.json") +
               glob.glob("**/vkbench-result.json", recursive=True),
               key=lambda p: os.path.getmtime(p), reverse=True)
    path = c[0] if c else None
if not path or not os.path.isfile(path):
    sys.exit("usage: bench-report.py <vkbench-result.json>")

d = json.load(open(path))
GIB = 1 << 30

print("=" * 60)
print("STAR BIONIC / vkbench")
print("=" * 60)
print(f"source   : {path}")
print(f"device   : {d.get('device','?')}")
print(f"loader   : {d.get('loader_path','?')}   instance API {d.get('instance_api_version','?')}")
if "error" in d:
    print(f"\nERROR: {d['error']}")
    sys.exit(0)

q = d.get("queues", {})
print("\n--- QUEUES (Stage 4) ---")
for f in q.get("families", []):
    caps = "+".join(k.upper() for k in ("graphics", "compute", "transfer", "sparse_binding") if f.get(k))
    print(f"  family{f['index']}  count={f['queueCount']}  tsBits={f['timestampValidBits']}  {caps}")
m = q.get("d3d12_mapping", {})
print(f"  graphics family queue count : {m.get('graphics_family_queue_count')}")
print(f"  separate queues possible    : {m.get('separate_queues_possible')}")
print(f"  -> {m.get('consequence','')}")

p = d.get("pipeline_creation", {})
print("\n--- PIPELINE CREATION (Stage 5) ---")
if "error" in p:
    print(f"  ERROR: {p['error']}")
else:
    print(f"  pipelines compiled     : {p.get('pipeline_count')}")
    print(f"  creation_feedback ext  : {p.get('pipeline_creation_feedback')}")
    for k, label in (("cold", "cold (empty cache)"),
                     ("warm_same_cache", "warm (same cache)"),
                     ("seeded_from_blob", "seeded (from blob)")):
        v = p.get(k)
        if isinstance(v, dict):
            drv = v.get("driver_reported_ms")
            drv = f"{drv:8.2f}" if isinstance(drv, (int, float)) else "     n/a"
            print(f"  {label:22} {v['wall_ms']:9.2f} ms  "
                  f"{v['wall_ms_per_pipeline']:6.2f}/pipe  driver {drv} ms  "
                  f"hits={v.get('cache_hits')}")
        else:
            print(f"  {label:22} (not run)")
    print(f"  cache blob             : {p.get('cache_blob_bytes')} bytes")
    sp = p.get("warm_speedup_x")
    if isinstance(sp, (int, float)):
        print(f"  warm speedup           : {sp:.2f}x")
    hits = (p.get("warm_same_cache") or {}).get("cache_hits")
    if hits == 0:
        print("  NOTE: cache_hits=0 -- the cache did nothing. Any wall-time")
        print("        drop is warm-up, not caching.")

mm = d.get("memory")
print("\n--- MEMORY (Stage 7) ---")
if not isinstance(mm, dict):
    print("  (skipped)")
elif "error" in mm:
    print(f"  ERROR: {mm['error']}")
else:
    print(f"  budget extension  : {mm.get('memory_budget_extension')}")
    print(f"  heap{mm.get('heap_index')} size       : {mm.get('heap_size_bytes',0)/GIB:.2f} GiB")
    print(f"  initial budget    : {mm.get('initial_budget_bytes',0)/GIB:.2f} GiB")
    print(f"  allocated total   : {mm.get('allocated_gib_total',0):.2f} GiB")
    print(f"  hit failure       : {mm.get('hit_allocation_failure')}   hit cap: {mm.get('hit_cap')}")
    print(f"  usage tracked     : {mm.get('usage_tracked_allocations')}"
          "   <- if False the budget is decorative")
    for s in mm.get("steps", []):
        print(f"    {s['allocated_mb']:6d} MB   budget {s['budget_bytes']/GIB:6.2f} GiB"
              f"   usage {s['usage_bytes']/GIB:6.2f} GiB")
    print(f"  after free        : budget {mm.get('budget_after_free_bytes',0)/GIB:.2f} GiB"
          f"   usage {mm.get('usage_after_free_bytes',0)/GIB:.2f} GiB")
print("=" * 60)

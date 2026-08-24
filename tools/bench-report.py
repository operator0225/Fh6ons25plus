#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render a vkbench result as a compact, paste-able summary."""
import json, re, sys, glob, os

def find_results(name="vkbench-result.json", max_depth=4):
    """
    Newest matching file, searched a few levels down the usual places.

    The exe writes its report beside itself, and people keep the exe in a
    subfolder of Downloads to find it easily -- so a flat glob of
    /sdcard/Download misses it. Walk, but bounded: Downloads can be large.
    """
    roots = ["/sdcard/Download", "/storage/emulated/0/Download",
             os.getcwd(), os.path.join(os.getcwd(), "measurements")]
    found = []
    for root in roots:
        if not os.path.isdir(root):
            continue
        base_depth = root.rstrip(os.sep).count(os.sep)
        for dirpath, dirnames, filenames in os.walk(root):
            if dirpath.count(os.sep) - base_depth >= max_depth:
                dirnames[:] = []
                continue
            dirnames[:] = [d for d in dirnames if not d.startswith(".")]
            if name in filenames:
                found.append(os.path.join(dirpath, name))
    return sorted(set(found), key=os.path.getmtime, reverse=True)


path = sys.argv[1] if len(sys.argv) > 1 else None
if path and not os.path.isfile(path):
    sys.exit(f"bench-report: no such file: {path}")
if not path:
    cands = find_results()
    if not cands:
        sys.exit("bench-report: no vkbench-result.json found under Downloads or here.\n"
                 "  Run vkbench.exe inside Winlator first -- it writes the report\n"
                 "  beside the exe, wherever you put it.\n"
                 "  Or pass the path explicitly.")
    path = cands[0]
    if len(cands) > 1:
        print(f"(newest of {len(cands)} found)")

raw = open(path).read()
try:
    d = json.loads(raw)
    truncated = False
except json.JSONDecodeError:
    # A container killed mid-write leaves valid JSON with the closing
    # brackets missing. That data is still worth reading, so close the open
    # structures and say so rather than refusing the file.
    text = raw.rstrip().rstrip(",")
    stack, in_str, esc = [], False, False
    for ch in text:
        if in_str:
            if esc:       esc = False
            elif ch == "\\": esc = True
            elif ch == '"':  in_str = False
        elif ch == '"':   in_str = True
        elif ch in "{[":  stack.append(ch)
        elif ch in "}]":
            if stack: stack.pop()
    if in_str:
        text += '"'
    # Drop a dangling `"key":` with no value.
    text = re.sub(r',?\s*"[^"]*"\s*:\s*$', "", text)
    text += "".join("}" if c == "{" else "]" for c in reversed(stack))
    try:
        d = json.loads(text)
        truncated = True
    except json.JSONDecodeError as e:
        sys.exit(f"bench-report: {path} is not valid JSON and could not be "
                 f"repaired ({e})")
GIB = 1 << 30

print("=" * 60)
print("STAR BIONIC / vkbench")
print("=" * 60)
print(f"source   : {path}")
print(f"device   : {d.get('device','?')}")
if truncated:
    print("\n  *** FILE WAS TRUNCATED -- recovered what was written. ***")
    print("  *** The run did not finish; the container was probably killed. ***")
print(f"loader   : {d.get('loader_path','?')}   instance API {d.get('instance_api_version','?')}")
gs0 = d.get("gpu_state_at_start")
if d.get("kgsl_sysfs_readable") and isinstance(gs0, dict):
    print(f"gpu state: {gs0.get('gpu_mhz')} MHz  {gs0.get('gpu_temp_c')} C  "
          f"throttling={gs0.get('throttling')}  busy={gs0.get('gpu_busy_pct')}%")
elif d.get("kgsl_sysfs_readable") is False:
    print("gpu state: KGSL sysfs not readable from inside the container")
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

qc = d.get("queue_cost")
print("\n--- QUEUE COST (Stage 4) ---")
if not isinstance(qc, dict):
    print("  (skipped)")
elif "error" in qc:
    print(f"  ERROR: {qc['error']}")
else:
    print(f"  iterations {qc.get('iterations')}  groups {qc.get('workgroups_per_dispatch')}"
          f"  copy {qc.get('copy_mb_per_iteration')} MB/iter"
          f"  gpu timestamps: {qc.get('gpu_timestamps')}")
    for k, label in (("render_only", "render only"),
                     ("upload_only", "upload only"),
                     ("interleaved", "interleaved")):
        v = qc.get(k, {})
        if "error" in v:
            print(f"  {label:14} ERROR: {v['error']}")
        else:
            g = v.get("gpu_ms")
            g = f"{g:8.2f}" if isinstance(g, (int, float)) else "     n/a"
            st = v.get("gpu_state") or {}
            extra = ""
            if st.get("gpu_mhz") is not None:
                extra = (f"   [{st.get('gpu_mhz')} MHz {st.get('gpu_temp_c')} C"
                         f" thr={st.get('throttling')}]")
            print(f"  {label:14} wall {v.get('wall_ms', 0):8.2f} ms   gpu {g} ms{extra}")
    ov = qc.get("overlap_fraction")
    if isinstance(ov, (int, float)):
        print(f"  basis          : {qc.get('overlap_basis')}")
        print(f"  serial sum     : {qc.get('serial_sum_ms'):8.2f} ms")
        print(f"  interleaved    : {qc.get('interleaved_ms'):8.2f} ms")
        print(f"  overlap        : {ov:.3f}   (0 = none, 1 = uploads free)")
        print(f"  upload cost    : {qc.get('upload_cost_ms'):8.2f} ms on top of render")
        print(f"  -> {qc.get('reading','')}")

fl = d.get("frame_loop")
print("\n--- GPU FRAME-TIME CURVE (Stage 8/10) ---")
if not isinstance(fl, dict):
    print("  (skipped)")
elif "error" in fl:
    print(f"  ERROR: {fl['error']}")
else:
    print(f"  {fl.get('__caveat','')}")
    print(f"  timestamp period: {fl.get('timestamp_period_ns')} ns")
    print("     groups    avg ms    max ms   spread    fps    60Hz avg/worst   GPU MHz / C")
    for L in fl.get("levels", []):
        g = L.get("gpu_ms_avg")
        if isinstance(g, (int, float)):
            mx = L.get("gpu_ms_max", g)
            sp = L.get("gpu_ms_spread_pct")
            sp = f"{sp:6.1f}%" if isinstance(sp, (int, float)) else "     ?"
            # Older captures only carry the averaged flag.
            fa = L.get("fits_60hz_avg", L.get("fits_60hz_budget"))
            fw = L.get("fits_60hz_worst")
            fw = ("yes" if fw else "NO") if fw is not None else "?"
            st = L.get("gpu_state_after") or {}
            gpu = (f"  {st.get('gpu_mhz')} / {st.get('gpu_temp_c')}"
                   if st.get("gpu_mhz") is not None else "")
            print(f"    {L['workgroups']:8d} {g:9.2f} {mx:9.2f} {sp} "
                  f"{L.get('implied_fps',0):7.0f}    {'yes' if fa else 'NO':>3} / {fw}{gpu}")
        else:
            print(f"    {L['workgroups']:8d}   (not measured)")
    ct = fl.get("groups_consistently_under_60hz")
    if isinstance(ct, (int, float)) and ct:
        print(f"  consistently under 60Hz up to : {ct:.0f} workgroups")
    th = fl.get("groups_at_60hz_budget")
    if isinstance(th, (int, float)):
        print(f"  average crosses 16.67 ms at   : {th:.0f} workgroups")
        print("  NOTE: the gap between those two is the region where the mean")
        print("        fits 60Hz but individual frames do not.")

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
    if mm.get("exceeded_initial_budget"):
        print("  NOTE: allocated MORE than the initial budget. budget is an")
        print("        estimate (usage + remaining), not a ceiling.")
    print("    allocated    budget    usage  headroom")
    for s in mm.get("steps", []):
        hr = s.get("headroom_bytes")
        hr = f"{hr/GIB:7.2f}" if isinstance(hr, int) else "      ?"
        print(f"    {s['allocated_mb']:6d} MB {s['budget_bytes']/GIB:8.2f} "
              f"{s['usage_bytes']/GIB:8.2f} {hr}")
    print("    (headroom = budget - usage; this is what actually falls)")
    print(f"  after free        : budget {mm.get('budget_after_free_bytes',0)/GIB:.2f} GiB"
          f"   usage {mm.get('usage_after_free_bytes',0)/GIB:.2f} GiB")
print("=" * 60)

# Per-process GPU usage

The **GPU %** column of the Processes page shows how busy the GPU is because of each process.
It is implemented in [src/os/processgpu.cpp](../src/os/processgpu.cpp) and is independent of the
system-wide GPU backends described in [amdsmi.md](amdsmi.md).

## What the number means

For every process the value is the utilization of its **busiest GPU engine** (3D, compute, copy,
video decode, ...) between two refreshes. The Windows Task Manager uses the same rule: its per-process
GPU column is the highest engine utilization, with the per-engine breakdown available separately.

Windows can do this for every vendor because the OS graphics scheduler (WDDM) accounts engine time
itself. Linux has no common scheduler, so the data has to come from the drivers. That is why this
feature has several sources and why coverage depends on the driver.

## Sources

| Source | Used for | How |
|---|---|---|
| DRM fdinfo | amdgpu, i915, xe, msm, panfrost, v3d, nouveau, NPUs under `/dev/accel` and any driver implementing the kernel's drm-usage-stats | Reads `/proc/PID/fdinfo/N` of every fd the process holds on `/dev/dri/*` or `/dev/accel/*` |
| NVML | Proprietary NVIDIA driver (and nvidia-open) | `dlopen("libnvidia-ml.so.1")`, `nvmlDeviceGetProcessUtilization`; the value is the maximum of SM, encoder and decoder utilization |

When a process has data from both, the larger value is shown.

### DRM fdinfo keys

Specified in the kernel's `Documentation/gpu/drm-usage-stats.rst`:

| Key | Meaning |
|---|---|
| `drm-engine-<name>: N ns` | Time the engine spent on this client. `utilization = delta ns / (elapsed ns x capacity)` |
| `drm-engine-capacity-<name>: N` | Number of identical instances of the engine (i915 video, for instance) |
| `drm-total-cycles-<name>` + `drm-cycles-<name>` | Cycle based drivers (xe). `utilization = delta cycles / delta total cycles` |
| `drm-client-id`, `drm-pdev` | Used to count a client once even if its fd is duplicated, and to keep GPUs apart |

`drm-cycles-<name>` **without** `drm-total-cycles-<name>` (msm, panfrost) is ignored and the
nanosecond counter is used instead.

## What the cell shows

| Cell | Meaning |
|---|---|
| `12.5 %` | Measured |
| `0.0 %` | Measured, the process does not use the GPU |
| `—` | Not measurable: no data source on this system, the driver exposes no engine counters, or the process has just appeared and has no previous sample |
| `?` | Another user's process whose GPU clients cannot be inspected without root (same as the I/O columns) |

A `0 %` is never shown for something that was not measured.

Sampling only runs while the GPU % column is visible. File descriptors of each process are cached and
re-scanned every 5 refreshes (staggered by PID), so a process that opens the GPU for the first time can
take up to 5 refreshes to show up.

## Checking what was detected

Start TuxManager and look for the line logged once the first two samples are in (and again if it changes):

```
INFO : Per-process GPU usage sources: DRM fdinfo [amdgpu: engine-ns]; NVML [not available] | measured=212 denied=0
```

* `DRM fdinfo [no /dev/dri or /dev/accel]`: the machine has no DRM device.
* `DRM fdinfo [no client exposes drm-* counters]`: DRM devices exist but no running process reports
  counters. This is what the proprietary NVIDIA driver looks like; NVML is then the source.
* `<driver>: no engine counters`: the driver or kernel is too old to report engine usage; the column shows `—`.
* `<driver>: engine-ns` / `cycles`: working.

To inspect a driver by hand:

```sh
for fd in /proc/$(pidof -s firefox)/fd/*; do
  case "$(readlink "$fd")" in /dev/dri/*|/dev/accel/*) cat "/proc/$(pidof -s firefox)/fdinfo/${fd##*/}";; esac
done
```

If there is no `drm-engine-*` / `drm-total-cycles-*` line, the kernel or driver does not provide the data
and nothing TuxManager can do will change that.

## Verification status

| GPU / driver | Source | Status |
|---|---|---|
| NVIDIA proprietary (GTX 1660 SUPER, Fedora 44, kernel 7.3) | NVML | Verified on hardware; `fdinfo` capture confirms the driver exposes no `drm-*` keys |
| AMD amdgpu | DRM `engine-ns` | Parser and math unit tested with a **synthetic** fixture. Not yet verified on hardware |
| Intel i915 | DRM `engine-ns` + capacity | Same as above |
| Intel xe | DRM `cycles` | Same as above |
| Qualcomm msm | DRM `engine-ns` | Same as above |
| nouveau, panfrost, v3d, NPUs | DRM `engine-ns` | Not covered by a fixture yet |

Synthetic fixtures are written from the kernel documentation and are marked as such in their first line.
They prove the code follows the documented format, not that a given driver and kernel behave like the
documentation.

## Unit tests

```sh
cd tests/processgpu
qmake6 processgpu.pro && make && ./processgpu_test
```

They run in CI. The fixtures are in `tests/fixtures/drm`.

### Contributing a capture from your hardware

1. Run TuxManager, open any GPU application, then run the shell snippet above.
2. Save the output as `tests/fixtures/drm/<driver>-<gpu>.fdinfo`. Add a first line starting with `#`
   saying that it is a real capture, with the GPU, kernel and distribution (the parser ignores it).
3. Add a test in `tests/processgpu/processgpu_test.cpp` that parses it, and update the table above.

Cross-check the percentages against `nvtop` (any GPU, same fdinfo mechanism), `intel_gpu_top`, `amdgpu_top`
or `nvidia-smi pmon` while a known load is running.

## Known limitations

* Needs the driver to implement the kernel's drm-usage-stats; there is no fallback for drivers that do not.
* Another user's processes need root for DRM sources (NVML sees all users).
* Processes in another PID namespace (containers, Flatpak, Snap) or with `/proc` mounted with `hidepid`
  may show `—` or `?`.
* A process using several GPUs shows its busiest engine across all of them.

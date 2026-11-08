# Retroid Pocket 5

Recorded on 6 November 2026 from the unit reached at `armada@192.168.188.119` (`armada.fritz.box`). That address is the LAN address of this session and can change. Readings below are from this unit, not from a generic RP5 datasheet.

The device tree model is `Retroid Pocket 5`. The compatible string is `retroidpocket,rp5 qcom,sm8250`. Armada selects the profile `retroid-pocket-5` from that model string. The Visionox panel profile (`retroid-pocket-5-visionox`) is a different device tree model and is not this unit.

## Operating system

| Item | Value |
| --- | --- |
| OS | Armada `20261103.183b2c3` (`VERSION_ID` 44, Fedora-like) |
| Image | `IMAGE_ID=armada`, build `20261103.183b2c3` |
| Kernel | `7.2.6 #1 SMP PREEMPT`, built Mon 2 Nov 2026 22:06:18 UTC, `aarch64` |
| Page size | 4096 |
| glibc | 2.43 (`glibc-2.43-9.fc44.aarch64`) |
| Hostname | `armada` |
| User | `armada` (uid 1000). Groups include `wheel`, `video`, `render`, `input`, `gamemode`, `seat` |
| Root filesystem | composefs, 35 MB, the immutable image |
| Home | btrfs on the internal UFS, mounted at `/var/home` (same tree as `/home`) |
| Suspend | `s2idle` |
| TuneD | active profile name `balanced` (this is the TuneD daemon, separate from Armada's own power profiles below) |
| `perf` | not installed. `kernel.perf_event_paranoid` is `2` |

Armada's own docs and issue tracker: [armadaos.dev](https://armadaos.dev/), [github.com/armada-os/armada](https://github.com/armada-os/armada/issues).

## CPU

Snapdragon 865 (`SM8250`). Eight cores, one NUMA node, 32-bit and 64-bit. L2 cache 1.8 MiB across 8 instances, L3 4 MiB. Armada's core lists for this SoC:

- Little, `ARMADA_LITTLE_CORES=0-3`: Kryo-4XX-Silver (Cortex-A55 class), policy `policy0`
- Big, `ARMADA_BIG_CORES=4-7`: Cortex-A77
- Prime, `ARMADA_PRIME_CORES=7`: the single fastest A77, policy `policy7`
- The other three A77 cores are `policy4` (cpus 4–6)
- `ARMADA_IRQ_CORES` is empty. The device profile says the A55s are too weak to take every IRQ, so IRQs are not pinned to the little cores

Frequency boost is disabled. At the time of the reading every governor was `performance` and every policy was sitting on its maximum clock. Scaling limits matched the hardware limits (no underclock was applied).

### policy0, cpus 0–3 (little)

Hardware and scaling range: 300.0–1804.8 MHz. Current clock: 1804.8 MHz.

Steps, MHz: 300.0, 403.2, 518.4, 614.4, 691.2, 787.2, 883.2, 979.2, 1075.2, 1171.2, 1248.0, 1344.0, 1420.8, 1516.8, 1612.8, 1708.8, 1804.8.

### policy4, cpus 4–6 (A77)

Hardware and scaling range: 710.4–2419.2 MHz. Current clock: 2419.2 MHz.

Steps, MHz: 710.4, 825.6, 940.8, 1056.0, 1171.2, 1286.4, 1382.4, 1478.4, 1574.4, 1670.4, 1766.4, 1862.4, 1958.4, 2054.4, 2150.4, 2246.4, 2342.4, 2419.2.

### policy7, cpu 7 (prime)

Hardware and scaling range: 844.8–2841.6 MHz. Current clock: 2841.6 MHz.

Steps, MHz: 844.8, 960.0, 1075.2, 1190.4, 1305.6, 1401.6, 1516.8, 1632.0, 1747.2, 1862.4, 1977.6, 2073.6, 2169.6, 2265.6, 2361.6, 2457.6, 2553.6, 2649.6, 2745.6, 2841.6.

`lscpu` flags on both core types: `fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp`.

Armada's medium underclock for SM8250, used by its Balanced profile when that profile is the one applied, would cap the clusters at 1804.8 / 1862.4 / 2073.6 MHz. That cap was not in effect during this reading.

## GPU

Adreno 650. Kernel devfreq device: `/sys/class/devfreq/3d00000.gpu`.

Governors the node offers: `userspace`, `performance`, `simple_ondemand`.

The active governor is `simple_ondemand`. Both `min_freq` and `max_freq` are `587000000`, so the node cannot leave 587 MHz. Three samples a second apart during the game, and an earlier idle sample, all read 587 MHz.

`min_freq` and `max_freq` are writable by group `wheel`. This user is in `wheel`.

### Operating points

The kernel table, in MHz:

305, 400, 441.6, 490, 525, 587, 650, 670, 700, 725, 750, 800, 855, 905, 925.

### Stock ceiling

`/usr/libexec/armada/device-env` sets `ARMADA_GPU_STOCK_MAX_MHZ=587` for `SM8250`. `armada-powerd` treats that as the stock maximum. In automatic GPU mode the highest clock it will request is that stock value, or a fraction of it. The steps from 650 MHz through 925 MHz are in the kernel table and above the stock ceiling the power daemon will set.

`gpu_stock_max()` picks the highest table entry that does not exceed 587 MHz, which is 587 MHz itself.

## Power policy

Two different "balanced" names exist on this machine.

TuneD's active profile is `balanced` (`tuned-adm active`). That is a general TuneD profile and it is not the file that sets the GPU window.

Armada's power daemon is `/usr/libexec/armada/armada-powerd`. Its profiles live in `/usr/share/armada/power-profiles.conf`. The default named there is `balanced`. The three profiles, applied against the 587 MHz stock GPU clock:

| Armada profile | CPU governor | CPU underclock | GPU min | GPU max |
| --- | --- | --- | --- | --- |
| Eco | `conservative` | large (SM8250 caps 1708.8 / 1478.4 / 1862.4 MHz) | 0 × stock | 0.80 × stock |
| Balanced | `conservative` | medium (1804.8 / 1862.4 / 2073.6 MHz) | 0 × stock | 1.0 × stock |
| Performance | `performance` | none | 1.0 × stock | 1.0 × stock |

`apply_gpu_auto()` writes `simple_ondemand`, then the resolved min and max. With Performance, both ends are 587 MHz, which is the window observed on this unit. The CPU governors observed (`performance`, scaling max equal to the hardware max on every policy) match that same Performance profile. The GPU was not held down by temperature: during the vista it was about 54°C, and a later sample the same afternoon was about 47°C, with the window still pinned.

The shared profile file also defines fan curves (relaxed, moderate, aggressive). No fan device was identified on this unit while collecting this note.

`armada-powerd` starts with `gpu_level=auto` and `manual_gpu_clock=0`. Manual mode would switch the governor to `userspace` and pin min and max to the chosen step, still capped by the manual maximum helper.

## Memory

| Item | Value |
| --- | --- |
| MemTotal | 7,781,260 kB (about 7.42 GiB usable on an 8 GB device) |
| Swap | zram0, 7,780,348 kB, active |
| CMA | 262,144 kB |

At one sample while the machine was up: MemFree about 4.4 GiB, MemAvailable about 5.0 GiB.

## Storage

Internal storage is a 128 GB UFS device reported as `eUFS3.1_128` (`sda` and several smaller UFS LUNs). The userdata partition is `sda26`, 78.1 GB btrfs, mounted at `/var/home`. At the reading: 27 GB used, 52 GB free.

The game card is `mmcblk0`, 476.8 GB, one ext4 partition mounted at `/run/media/armada/SD128`. At the reading: 469 GB filesystem, 433 GB used, 37 GB free (93%). The mount name `SD128` is the volume label path, not the card's size.

`/boot` is ext4 on `sda25` (1 GB). `/boot/efi` is vfat on `sda24` (512 MB).

## Display

The panel connector is `card1-DSI-1`, status `connected`, enabled. Its only mode is `1080x1920` (portrait).

Armada's device profile:

- `ARMADA_PRIMARY_CONNECTOR=DSI-1`
- `ARMADA_PANEL_TYPE=internal`
- `ARMADA_PANEL_ORIENTATION=left`
- `ARMADA_HDR_NITS=580`
- `ARMADA_GAMESCOPE_USE_ROTATION_SHADER=0`
- Backlight sysfs: `ae94000.dsi.0`, brightness 66 of 255 at the time of the reading

Gamescope is the session compositor. The running command:

```
/usr/bin/gamescope --force-orientation left --custom-refresh-rates 60
  --hdr-itm-target-nits 580 --fake-output-mm 177x100
  --prefer-output *,DSI-1 --xwayland-count 2 --default-touch-mode 4
  --hide-cursor-delay 3000 --fade-out-duration 200 --steam
```

What clients see, from `DISPLAY=:0 xrandr`:

- Output name `gamescope`
- Current mode `1920x1080` at `59.96 Hz`, marked preferred
- Reported physical size `177 mm × 100 mm` (the fake size passed on the command line)
- A long list of extra modes, all near 60 Hz, from `320x240` up to `1920x1080`

Two rootless Xwayland servers are running (`:0` and `:1`). The session is started by SDDM as `gamescope-session-plus steam`, which runs `/usr/bin/gamescope-session-plus` and then Steam with `-gamepadui -steamos3 -steampal -steamdeck`. Steam's client beta file selects `steamdeck_publicbeta`. The client build id observed in the log was `1793740892`.

Package versions:

- `terra-gamescope-3.17.0^1-1.103013.fc44.armada`
- `gamescope-session` git snapshot `20261002`, `gamescope-session-steam` git snapshot `20260919`

## Graphics stack

Vulkan loader `1.4.341.0-1.fc44`. Tools package `vulkan-tools` is installed (`vulkaninfo`).

GPU 0, the device the game uses:

| Property | Value |
| --- | --- |
| deviceName | Turnip Adreno (TM) 650 |
| driverID | `DRIVER_ID_MESA_TURNIP` |
| apiVersion | 1.3.354 |
| driverInfo | Mesa 26.2.6 |
| driver package | `mesa-vulkan-drivers-26.2.6-1.fc44.armada.aarch64` |
| deviceID | `0x6050002` |
| vendorID | `0x5143` (Qualcomm) |
| deviceType | integrated GPU |
| conformance | 1.2.7.1 |
| maxImageDimension2D | 16384 |
| maxFramebufferWidth | 16384 |
| maxStorageBufferRange | 134217728 (128 MB) |
| maxBoundDescriptorSets | 4 |
| maxMemoryAllocationSize | `0xffffffff` |
| timestampPeriod | 52.0833 ns |
| shaderInt64 / shaderInt16 | both true |

Turnip on this machine advertises `VK_EXT_map_memory_placed`, `VK_KHR_map_memory2`, `VK_KHR_timeline_semaphore`, `VK_EXT_descriptor_buffer`, `VK_EXT_attachment_feedback_loop_layout` (revision 2), and `VK_EXT_attachment_feedback_loop_dynamic_state`.

GPU 1 is `llvmpipe (LLVM 22.1.8, 128 bits)`, Mesa 26.2.6, Vulkan 1.4.354. It is a software device. Gamescope and the game both see it. A correct run logs `using Turnip Adreno (TM) 650`.

System SDL2 is not installed. The game ships its own SDL3 (`libSDL3.so.0.4.16` under the game's `lib/`).

## Thermals and battery

Temperatures are millidegrees in sysfs. Two samples on 6 November 2026:

During the opening-vista warmup, GPU top/bottom about 53.6°C / 54.3°C, CPU clusters about 56–59°C.

A later sample the same afternoon, machine still on, game no longer the focus of the sample: GPU top 46.7°C, GPU bottom 46.9°C, CPU clusters about 49–50°C, battery 36.0°C. A few zones (`skin-msm-thermal`, `pm8150l-pcb-thermal`) read −40°C, which is an empty sensor rather than a real temperature.

Zones present include `cpu0`–`cpu7` top and bottom, `cluster0`, `cluster1`, `gpu-top`, `gpu-bottom`, `aoss0`, `aoss1`, `mem`, `video`, `compute`, `npu`, `wlan`, `wifi`, `battery`, and the PMIC sensors `pm8150`, `pm8150b`, `pm8150l`.

Battery at that later sample: status `Full`, capacity 100, `voltage_now` 4,381,580 µV. Other supplies: `pm8150b-charger`, a USB-C `tcpm` source, and a PlayStation controller battery (`ps-controller-battery-42:2f:a4:d5:05:02`).

## How a game is launched

The Steam session is Game Mode through gamescope. Non-Steam shortcuts live in:

`/var/home/armada/.local/share/Steam/userdata/75048018/config/shortcuts.vdf`

Each shortcut's launch options on this unit are:

```
/usr/libexec/armada/armada-game-launch %command%
```

`armada-game-launch` is a Python prefix. It applies FEX, Turnip, and per-game performance settings, then `exec`s the game. For a non-Steam shortcut, `STEAM_COMPAT_APP_ID` is `0`, and the wrapper then has no app id, so entries in `/etc/armada/game-tweaks.json` keyed by a Steam app id do not apply. The only game in that file today is Cult of the Lamb (`1313140`). Global tweaks set `turnipDriver` to `stable`.

`SM8250` Proton default, for x86 titles, is `proton-cachyos-11.0-arm64`.

Stderr from a game launched this way is captured under:

`/var/home/armada/.local/share/armada/logs/`

Files are named `steam-launch-<date>-<time>.log`. The Steam client itself also writes there, so a new game does not always create a new file. The 6 November session appended to `steam-launch-20261106-111010.log`.

Controller target list from the device profile: `deck-uhid,xbox-series,xb360,ds5`. Gamepad quirk: `none`.

## This game on this device

Install directory:

`/run/media/armada/SD128/BotwRecomp-linux-aarch64/`

| File | Role |
| --- | --- |
| `botwrecomp` | Launch path Steam uses. As of this note it is a shell wrapper that exports the audit environment and execs `botwrecomp.bin`. The kernel follows the shebang. A later re-exec via `/proc/self/exe` keeps the environment and runs the ELF directly. |
| `botwrecomp.bin` | The ELF, aarch64, dynamically linked, BuildID `8ce13f6946030c9c27f0c5ffe6ba5e73f95cfb8e`, with debug info, not stripped. Size about 7.5 MB. Dated 6 Nov 2026 11:48 on the card. |
| `botwgame.so` | Recompiled game code, about 118 MB, plus `botwgame.so.id` |
| `game_version.txt` | `190` (game 1.9.0) |
| `v190/extracted` | exefs, romfs, and a copy of `botwgame.so` |
| `extracted/` | Empty marker that makes this folder the data directory |
| `lib/` | Bundled SDL3 and shaderc. Vulkan comes from the system Turnip ICD, not from this directory |
| `v190/botwrecompandroid.ini` | Video and control settings |
| `v190/cache/vk_pipelines.bin` | Pipeline cache, about 17 MB after the warmup boot |

Steam shortcut name `botwrecomp`, exe and start directory as above.

The audit wrapper for the next launch exports:

```
LARECOMP_VK_PROF=1
LARECOMP_FPS_INTERVAL=2
LARECOMP_PACE=0
LARECOMP_FPS=60
LARECOMP_GPUPROF_AT=2000
```

The timed baseline used the same variables plus `LARECOMP_LOG_SCENE=1`, and did not set `LARECOMP_GPUPROF_AT`.

`LARECOMP_NO_LAUNCHER` is not set.

Settings in `v190/botwrecompandroid.ini` after the user set internal resolution to 1x:

```
aspect = original
fullscreen = 0
upscaler = bilinear
output = 720
resolution = 1x
show_fps = 1
```

Volume, reverb, and movie audio are at their defaults. Language is `auto`.

What the running process reported on 6 November 2026. The shader warmup and the later timed boot agreed. Figures below are the timed boot, the still opening vista after compiles had gone to zero, about four minutes of that shot (the requested 90 seconds are inside it):

- Device line: `using Turnip Adreno (TM) 650`
- Swapchain: `1280x720`, 4 images, `VK_FORMAT_B8G8R8A8_UNORM`. Gamescope's WSI logged `flip: true` and `presentTiming: off`
- Scene size at 1x: viewport `1600x900` of a `1600x900` target. A second viewport of `800x450` is also logged each frame
- Guest heap: `3120 MB is GPU memory (placed at 0x7240000000, align 65536)`. Draws reach shader global memory through windows, because the 128 MB storage-buffer limit cannot cover that heap
- Page write tracking did not start (`no page write tracking (-1, page 4096)`), and the vertex cache stayed off for that reason
- Feedback: `targets a draw writes, sampled by it: copies (off)`. In-place reads stay off on this Linux build unless `LARECOMP_FEEDBACK_INPLACE=1`. The extension itself is present on the driver
- Steady vista: 15.1 fps, 66.2 ms a frame, worst frame 77 ms. GPU timestamps 43.0 ms, command translation 19.0 ms. Those sum to 62.0 ms, so the GPU is idle while the CPU translates. The fence wait was 38.9 ms and sits inside the GPU time. About 4313 draws, 175 passes, 20 dispatches, 12 feedback copies, 179 feedback reads, and 14.7 MB uploaded every frame. GPU clock stayed at 587 MHz, GPU about 53–55°C, CPU clusters at their maximum clocks (about 58–59°C)
- One later boot recorded GPU time per pass for frame 2000 at 1x. That frame measured 44.9 ms, in line with the 43 ms average, still at 587 MHz. Of it: 17.5 ms in the 1600×900 G-buffer (2967 draws, one pass alone 15.1 ms and 2671 draws), 5.6 ms in other 1600×900 scene passes (33 draws), 4.9 ms in depth-only passes (724 draws), 4.4 ms at 800×450, 3.6 ms in twelve 1600×900 feedback copies, 2.2 ms in targets at 1920×1080 or 2400×1140, and 0.2 ms to present the 1280×720 window
- With `resolution = 50%` on a following boot, same vista, same 587 MHz cap, GPU about 45–47°C: 19.9 fps over 48 seconds (50 ms a frame, worst 79 ms on the profiled frame). GPU time 28.1 ms, command translation 19.2 ms. Draw count stayed about 4500 and passes about 178. The main G-buffer was still a 1600×900, 6-target pass (12.3 ms, 2730 draws). The time that left was the other 1600×900 scene passes (5.6 ms down to 1.0 ms), the twelve feedback copies (3.6 ms down to 0.6 ms), and the 800×450 passes (4.4 ms down to 1.9 ms). Depth-only stayed about 4.7 ms

`perf` is still not installed, so a CPU sample of `botwrecomp.bin` versus `botwgame.so` is not possible until that package is added. The runtime binary has debug info. `botwgame.so` was built without it.

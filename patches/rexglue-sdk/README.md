# ReXGlue SDK patches

This port is built against the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD-3-Clause) at commit
`bd833a2ab69d305f99ddf57418fc38ffec8af93d` (2026-10-01, the nightly 0.10.0.24), with the nine patches in this
folder applied and the Vulkan backend switched on. The binaries in the release (`rexruntime.dll`,
`rexgpu-xenos.dll`) are built from exactly this.

| Patch | What it does | Why |
|---|---|---|
| 0001 | **Shared memory: streaming blocks** (`shared_memory_streaming_blocks`). A 64-page block of guest memory that is invalidated 8+ times in a second stops being write-protected and is re-uploaded on every request instead. | The game writes per-draw vertex data just ahead of the GPU, so the same pages bounced between upload, protect and access fault, which is slow, especially under Wine/Proton. |
| 0002 | **Command processor: precise `WAIT_REG_MEM` poll step** (`gpu_wait_reg_mem_sleep_us`). Waits in high-resolution steps instead of `Sleep(1)`. | `Sleep(1)` can take 15.6 ms at Windows' default timer resolution: a whole frame. |
| 0003 | **Input: re-enumerate devices at most every `input_device_refresh_ms`**; skip mouse-capture posts that change nothing. | Every input read re-enumerated every driver and contended with the UI thread. |
| 0004 | **Memory: free-page search hints in `BaseHeap::AllocRange`.** | Physical allocations scanned the page table linearly every time. |
| 0005 | **SDL input: cache each pad's joystick instance id.** | Draining queued axis events took the SDL joystick lock once per event. |
| 0006 | **Shared memory: allow streaming blocks with the scaled-resolve global watch.** | Without it 0001 was off whenever the render resolution is scaled up (always, on a PC). |
| 0007 | **Filesystem: optional read filter for host files** (`rex::filesystem::SetFileReadFilter`). | Lets an app serve modified game data without touching the files on disk (used by an early version of the random mazes option; kept for mods). |
| 0008 | **Graphics: optional app draw filter** (`rex::graphics::SetDrawRegisterFilter`). Called before each draw; may apply temporary register writes (e.g. a shader constant) for that draw only. | Recolours the title logo while the random mazes option is on. |
| 0009 | **Runtime: game data can be a package file (STFS).** A file given as the game data root is mounted read-only as an STFS package (LIVE / CON / PIRS). | Run the game straight from the package dumped from the console. |

Patches 0001–0006 are performance work, measured on Windows and on a Steam Deck under Proton; 0007–0009 add
small, general-purpose extension points. All are self-contained and could be offered upstream.

## Building the SDK with these patches

```
git clone https://github.com/rexglue/rexglue-sdk.git rexglue-sdk-src
cd rexglue-sdk-src
git checkout bd833a2ab69d305f99ddf57418fc38ffec8af93d
git submodule update --init --recursive
git am <this folder>/*.patch
```

On Windows, Git checks out libmspack's symlinks (`thirdparty/libmspack/cabextract/mspack/*`) as small text files;
replace each with a copy of the file it names. Then, inside a Visual Studio x64 developer prompt (`vcvars64.bat`)
with Visual Studio's LLVM `bin` folder on `PATH`:

```
cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DCMAKE_INSTALL_PREFIX=<install dir>
cmake --build out/build/win-amd64 --config Release --target install --parallel
```

Then configure this project's `rexglue` folder with `-DCMAKE_PREFIX_PATH=<install dir>` (see the main README).

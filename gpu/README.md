# The GPU under Linux

Mali-G57 (Valhall, arch 9.0.9 r0p1) at 23100000.gpu, with Android's own driver on both sides: the kernel's
`mali_kbase.ko` (vendor_dlkm, DDK r49p1; its unisoc_mm_emem, sprd_sip_svc and trusty are already up for the modem)
and the userspace `libGLES_mali.so` (EGL 1.5, OpenGL ES 3.2, Vulkan) in the modem's bionic chroot
(`/opt/mu5358/android`). Android 15's libGLES_mali.so exports no OpenCL any more (mu300's Android 13 one did), so
mu300's cltest cannot link; `gltest` is the test here.

Nothing loads the GPU at boot: `gpu-run` inserts mali_kbase the first time (devfreq 384-950 MHz, runtime PM powers
it off when idle), puts /dev/mali0 and the dma heaps into the chroot's /dev and runs the program there.

    /opt/mu5358/bin/gpu-run /opt/gltest          # EGL/GLES probe and one verified frame (pbuffer, no SurfaceFlinger)
    /opt/mu5358/bin/gpu-run /opt/gltest loop 60  # 60 s stress, frames/s

2026-10-30 on the board: `GL: ARM | Mali-G57 | OpenGL ES 3.2 v1.r49p1-04eac0`, the 64x64 frame right; 60 s stress
6157 frames (512x512, a 64-step shader) at 950 MHz, SoC 36 -> 40 °C, no kernel message but DVFS.

| File | |
|---|---|
| pull-gpu.sh | from a running Android: libGLES_mali.so and its library closure (readelf) into work/android-subset (not in the repository: proprietary) |
| gpu-run | on the board: load mali_kbase, the nodes into the chroot, run a bionic program |
| gltest.c, crt_android.c, build.sh | the test, for bionic with clang + lld, no NDK (`docker run --rm -v "$PWD":/w ubuntu:24.04` with clang lld: `sh /w/gpu/build.sh /w/work/android-subset`) |
| cltest, cltest.c | mu300's OpenCL test (Android 13 only) |

The screen UI (u30ui) does not use the GPU: 240x240 is cheaper on the CPU than a GLES context (~5 MB RAM).

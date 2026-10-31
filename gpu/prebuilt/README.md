# gltest, built

`gltest` is `../gltest.c` + `../crt_android.c` built by `../build.sh` against the device's own Android libraries
(`libGLES_mali.so`, bionic's `libc.so`, `libdl.so`: `work/android-subset`, never in this repository), 11 KB of this
project's code with those libraries as its dependencies. The Release workflow has no Android tree to link against,
so it takes this copy; a maintainer with the tree rebuilds it with `gpu/build.sh` and puts the result here.

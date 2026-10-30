# Native arm64 build environment for the U30 Pro's GKI kernel (android15-6.6; the device's kernel was built with
# Android clang 18.0.0, r510928)
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    clang-18 lld-18 llvm-18 build-essential bc bison flex libssl-dev libelf-dev dwarves \
    python3 cpio git kmod ca-certificates rsync lz4 xz-utils file openssl \
 && rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/lib/llvm-18/bin:$PATH
WORKDIR /src

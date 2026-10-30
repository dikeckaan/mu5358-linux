#!/bin/bash
# Build spispeed.ko and panelcmd.ko against the board's kernel (kernel/build.sh's tree on the mu5358kb volume) and
# sign them with its key:
#   docker run --rm -v /Volumes/mu5358kb:/src -v "$PWD/tools/spispeed":/m mu5358-kbuild bash /m/build.sh
set -e
cd /src/common
make O=/src/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=/m modules
for k in /m/*.ko; do
    /src/out/scripts/sign-file sha1 /src/out/certs/signing_key.pem /src/out/certs/signing_key.x509 "$k"
done
ls -la /m/*.ko

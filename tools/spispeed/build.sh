#!/bin/bash
# Build spispeed.ko and panelcmd.ko against the board's kernel (kernel/build.sh's tree on the mu5358kb volume) and
# sign them with its key:
#   docker run --rm -v /Volumes/mu5358kb:/src -v "$PWD/tools/spispeed":/m mu5358-kbuild bash /m/build.sh
# (SRC and M from the environment for other mounts, as the Release workflow runs it)
set -e
SRC=${SRC:-/src}; M=${M:-/m}
cd $SRC/common
make O=$SRC/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$M modules
for k in $M/*.ko; do
    $SRC/out/scripts/sign-file sha1 $SRC/out/certs/signing_key.pem $SRC/out/certs/signing_key.x509 "$k"
done
ls -la $M/*.ko

#!/bin/sh
# Builds the btmonitor ELF: watches the PS5's Bluetooth without touching it.
set -e
SDK="${1:?usage: $0 /path/to/ps5-payload-sdk}"
IMAGE="${2:-anypad-build}"
VERSION="$(sed -n 's/^#define ANYPAD_VERSION "\(.*\)"/\1/p' btmonitor_version.h 2>/dev/null || echo dev)"
docker run --rm --platform linux/amd64 \
  -v "$SDK":/opt/ps5-payload-sdk-ro:ro -v "$PWD":/work "$IMAGE" bash -c '
set -e
cp -r /opt/ps5-payload-sdk-ro /opt/ps5-payload-sdk
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
printf "#!/bin/sh\ncase \"$1\" in --bindir) echo /usr/lib/llvm-18/bin;; --version) echo 18.1.3;; *) exit 1;; esac\n" > /usr/local/bin/llvm-config
chmod +x /usr/local/bin/llvm-config
CC=$PS5_PAYLOAD_SDK/bin/prospero-clang
cd /work
$CC -std=c11 -Wall -Wextra -Werror -O2 -Isrc -o dist/btmonitor.elf src/btmonitor.c \
  -lSceSystemService -ldl -lpthread
echo BUILD_OK
'

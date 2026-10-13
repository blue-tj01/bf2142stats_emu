#!/bin/sh
# Builds a private static OpenSSL with SSLv3 and weak ciphers (RC4 etc.)
# enabled, as needed to intercept the game's FESL SSL connections.
# Distribution OpenSSL packages are built without SSLv3.
#
# The result is installed in ./openssl-ssl3 and is used by "make ssl3".
# Never use this OpenSSL build for anything else.

set -e

VER=1.1.1w
SHA256=cf3098950cb4d853ad95c0841f1f9c6d3dc102dccfcacd521d93925208b76ac8
URL=https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/openssl-$VER.tar.gz

cd "$(dirname "$0")"
PREFIX="$(pwd)/openssl-ssl3"

if [ -f "$PREFIX/lib/libssl.a" ]; then
    echo "OpenSSL with SSLv3 already built in $PREFIX"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Downloading OpenSSL $VER"
curl -fsSL -o "$WORK/openssl.tar.gz" "$URL"
echo "$SHA256  $WORK/openssl.tar.gz" | sha256sum -c -

tar -xzf "$WORK/openssl.tar.gz" -C "$WORK"
cd "$WORK/openssl-$VER"
./config --prefix="$PREFIX" --openssldir="$PREFIX/ssl" \
    no-shared no-tests enable-ssl3 enable-ssl3-method enable-weak-ssl-ciphers
make -j"$(nproc 2>/dev/null || echo 2)"
make install_sw

echo "OpenSSL with SSLv3 installed in $PREFIX"

#!/usr/bin/env bash
set -euo pipefail

# -------- config (overridable via env) --------------------------------------
ARCH="${ARCH:-$(uname -m)}" # x86_64 | aarch64 | s390x | ppc64le
GPG_DIR="gnupg-2.5.16"
OUT_DIR="${OUT_DIR:-$(pwd)/dist}"
PLATFORM="${PLATFORM:-}"         # e.g. linux/arm64 if you want to force
DOCKER_IMAGE=""                  # filled below
CPU_BASELINE="${CPU_BASELINE:-}" # default per-arch below

# Map arch -> image + defaults
case "$ARCH" in
x86_64 | amd64)
    ARCH=x86_64
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_x86_64"
    CPU_BASELINE="${CPU_BASELINE:-x86-64}" # or x86-64-v2 / v3
    ;;
aarch64 | arm64)
    ARCH=aarch64
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_aarch64"
    CPU_BASELINE="${CPU_BASELINE:-generic}"
    ;;
s390x | 390x)
    ARCH=s390x
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_s390x"
    CPU_BASELINE="${CPU_BASELINE:-generic}"
    ;;
ppc64le | ppc)
    ARCH=ppc64le
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_ppc64le"
    CPU_BASELINE="${CPU_BASELINE:-generic}"
    ;;
*)
    echo "Unsupported ARCH='$ARCH'. Expected x86_64|aarch64|s390x|ppc64le." >&2
    exit 1
    ;;
esac

mkdir -p "$OUT_DIR"

echo "==> Build gpg for manylinux2014 ($ARCH)"
echo "    Image: $DOCKER_IMAGE"
echo "    CPU_BASELINE: $CPU_BASELINE"
[ -n "$PLATFORM" ] && echo "    docker --platform: $PLATFORM"

# Compose optional --platform flag
PLATFORM_ARGS=()
[ -n "$PLATFORM" ] && PLATFORM_ARGS=(--platform "$PLATFORM")

MY_LD_FLAGS="-Wl,-rpath,\$\$ORIGIN/../libs -Wl,--enable-new-dtags"

docker run --rm -t "${PLATFORM_ARGS[@]}" \
    -v "$OUT_DIR":/out \
    "$DOCKER_IMAGE" \
    bash -lc \
    '
    set -euo pipefail

    echo "==> glibc baseline:"
    ldd --version > /tmp/lddv && head -1 /tmp/lddv

    mkdir mongo_gpg
    cd mongo_gpg
    GPG_ROOT_DIR=$(pwd)
    mkdir gpg_bundle
    mkdir gpg_bundle/bin
    mkdir gpg_bundle/libs

    GPG_BUNDLE_DIR=$GPG_ROOT_DIR/gpg_bundle

    BUNDLE_BIN_DIR=$GPG_BUNDLE_DIR/bin
    BUNDLE_LIBS_DIR=$GPG_BUNDLE_DIR/libs

    # download key
    echo "Downloading signing key"
    curl -fL -o signature_key.asc https://gnupg.org/signature_key.asc
    echo "Importing signing key"
    if ! out=$(gpg --batch --no-tty --import signature_key.asc 2>&1); then
    echo "Ignoring keys without valid self-signed UIDs during import"
    fi

    verify_gpg_sig() {
        local artifact="$1" # e.g., libgpg-error-1.58.tar.bz2
        local sig_url="$2" # e.g., https://gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.58.tar.bz2.sig
        local sig_file="${3:-$(basename "$artifact").sig}" # optional override for sig filename
        echo "Verifying $sig_file"
        curl -fL -o "$sig_file" "$sig_url"
        if ! gpg --batch --no-tty --verify "$sig_file" "$artifact"; then
        echo "Signature verification failed for $artifact"
        exit 1
        fi
    }

    # libgpg-error
    echo "Downloading gpg-error"
    curl -fL -o libgpg-error-1.58.tar.bz2 https://www.gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.58.tar.bz2
    #verify_gpg_sig "libgpg-error-1.58.tar.bz2" "https://gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.58.tar.bz2.sig"

    tar -xvf libgpg-error-1.58.tar.bz2
    echo "Making gpg-error"

    cd libgpg-error-1.58
    ./configure
    make -j20
    make install
    cp src/.libs/libgpg-error.so.0.41.1 $BUNDLE_LIBS_DIR/libgpg-error.so.0

    # libgpgcrypt
    cd $GPG_ROOT_DIR
    echo "Downloading gpgcrypt"

    curl -fL -o libgcrypt-1.11.2.tar.bz2 https://www.gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.11.2.tar.bz2
    #verify_gpg_sig "libgcrypt-1.11.2.tar.bz2" "https://gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.11.2.tar.bz2.sig"
    
    tar -xvf libgcrypt-1.11.2.tar.bz2
    cd libgcrypt-1.11.2
    ./configure
    make -j20
    make install
    cp src/.libs/libgcrypt.so.20.6.0 $BUNDLE_LIBS_DIR/libgcrypt.so.20 

    echo "Downloading libksba"

    # libksba
    cd $GPG_ROOT_DIR
    curl -fL -o libksba-1.6.7.tar.bz2 https://www.gnupg.org/ftp/gcrypt/libksba/libksba-1.6.7.tar.bz2
    #verify_gpg_sig "libksba-1.6.7.tar.bz2" "https://gnupg.org/ftp/gcrypt/libksba/libksba-1.6.7.tar.bz2.sig"
    
    tar -xvf libksba-1.6.7.tar.bz2
    cd libksba-1.6.7
    ./configure
    make -j20
    make install
    cp src/.libs/libksba.so.8.14.7 $BUNDLE_LIBS_DIR/libksba.so.8

    # libassuan
    cd $GPG_ROOT_DIR
    echo "Downloading libassuan"

    curl -fL -o libassuan-3.0.2.tar.bz2 https://www.gnupg.org/ftp/gcrypt/libassuan/libassuan-3.0.2.tar.bz2
    #verify_gpg_sig "libassuan-3.0.2.tar.bz2" "https://gnupg.org/ftp/gcrypt/libassuan/libassuan-3.0.2.tar.bz2.sig"

    tar -xvf libassuan-3.0.2.tar.bz2
    cd libassuan-3.0.2
    ./configure
    make -j20
    make install
    cp src/.libs/libassuan.so.9.0.2 $BUNDLE_LIBS_DIR/libassuan.so.9

    # ntbtls
    echo "Downloading ntbtls"
    cd $GPG_ROOT_DIR
    curl -fL -o ntbtls-0.3.2.tar.bz2 https://www.gnupg.org/ftp/gcrypt/ntbtls/ntbtls-0.3.2.tar.bz2
    #verify_gpg_sig "ntbtls-0.3.2.tar.bz2" "https://gnupg.org/ftp/gcrypt/ntbtls/ntbtls-0.3.2.tar.bz2.sig"

    tar -xvf ntbtls-0.3.2.tar.bz2
    cd ntbtls-0.3.2
    ./configure
    make -j20
    make install
    cp src/.libs/libntbtls.so.0.1.3 $BUNDLE_LIBS_DIR/libntbtls.so.0

    # npth
    echo "Downloading npth"
    cd $GPG_ROOT_DIR
    curl -fL -o npth-1.8.tar.bz2 https://www.gnupg.org/ftp/gcrypt/npth/npth-1.8.tar.bz2
    #verify_gpg_sig "npth-1.8.tar.bz2" "https://gnupg.org/ftp/gcrypt/npth/npth-1.8.tar.bz2.sig"

    tar -xvf npth-1.8.tar.bz2
    cd npth-1.8
    ./configure
    make -j20
    make install
    cp src/.libs/libnpth.so.0.3.0 $BUNDLE_LIBS_DIR/libnpth.so.0
    
    # gpg
    cd $GPG_ROOT_DIR
    echo "Downloading gpg"
    curl -fL -o gnupg-w32-2.5.16_20251230.tar.xz https://www.gnupg.org/ftp/gcrypt/gnupg/gnupg-w32-2.5.16_20251230.tar.xz
    # verify_gpg_sig "gnupg-w32-2.5.16_20251230.tar.xz" "https://gnupg.org/ftp/gcrypt/gnupg/gnupg-w32-2.5.16_20251230.tar.xz.sig"
    
    tar -xvf gnupg-w32-2.5.16_20251230.tar.xz
    echo "making gpg"

    cd gnupg-w32-2.5.16
    echo "Currently in path $(pwd)"
    GPG_SRC_DIR=$(pwd)
    echo $GPG_SRC_DIR
    mkdir build
    cd build
    echo "Currently in path $(pwd)"
    echo "running configure on path $GPG_SRC_DIR"

    $GPG_SRC_DIR/configure --disable-sqlite

    make -j20 LDFLAGS='"'"'-Wl,-rpath,\$$ORIGIN/../libs -Wl,--enable-new-dtags'"'"'

    cp -L bin/gpg $BUNDLE_BIN_DIR/gpg
    cp -L bin/gpg-agent $BUNDLE_BIN_DIR/gpg-agent
    cp -L bin/gpg-card $BUNDLE_BIN_DIR/gpg-card
    cp -L bin/gpgconf $BUNDLE_BIN_DIR/gpgconf
    cp -L bin/gpgconf.ctl $BUNDLE_BIN_DIR/gpgconf.ctl
    cp -L bin/gpg-connect-agent $BUNDLE_BIN_DIR/gpg-connect-agent
    cp -L bin/gpgsm $BUNDLE_BIN_DIR/gpgsm
    cp -L bin/gpgtar $BUNDLE_BIN_DIR/gpgtar
    cp -L bin/gpgv $BUNDLE_BIN_DIR/gpgv
    
    OUT_NAME=gpg_bundle-'"$ARCH"'
    cp -r $GPG_BUNDLE_DIR /out/$OUT_NAME
    '

echo "Built: $OUT_DIR/gpg_bundle-$ARCH"

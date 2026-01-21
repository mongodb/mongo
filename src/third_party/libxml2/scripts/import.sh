#!/bin/bash
# This script downloads and imports libxml2

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=libxml2
VERSION="2.15.1"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/libxml2
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

TEMP_DIR=$(mktemp -d /tmp/import-libxml2.XXXXXX)
trap "rm -rf $TEMP_DIR" EXIT

# Download libxml2 release tarball
TARBALL_URL="https://download.gnome.org/sources/libxml2/2.15/libxml2-${VERSION}.tar.xz"
wget -O "$TEMP_DIR/libxml2.tar.xz" "$TARBALL_URL"

# Extract the tarball
tar -xf "$TEMP_DIR/libxml2.tar.xz" -C "$TEMP_DIR"

# Move to dist directory
mkdir -p "$DEST_DIR/dist"
mv "$TEMP_DIR/libxml2-${VERSION}"/* "$DEST_DIR/dist/"

# Generate platform-specific configuration headers
HOST_OS="$(uname -s | tr A-Z a-z)"
HOST_ARCH="$(uname -m)"
# Platform headers go inside dist/ so they're part of the module root
HOST_DIR="$DEST_DIR/dist/platform/${HOST_OS}_${HOST_ARCH}"

TOOLCHAIN_ROOT=/opt/mongodbtoolchain/v5
PATH="$TOOLCHAIN_ROOT/bin:$PATH"

SRC_DIR=${DEST_DIR}/dist
pushd $SRC_DIR

# Generate configure script if needed
if [[ ! -f configure ]]; then
    autoreconf -fi
fi
popd

mkdir -p "$HOST_DIR/build_tmp"
pushd "$HOST_DIR/build_tmp"

# Configure libxml2 with minimal features needed for Azure SDK
$SRC_DIR/configure \
    --prefix="$HOST_DIR/install_tmp" \
    --without-python \
    --without-lzma \
    --without-zlib \
    --without-iconv \
    --without-icu \
    --without-http \
    --without-ftp \
    --without-catalog \
    --without-docbook \
    --without-schematron \
    --with-xpath \
    --with-tree \
    --with-output \
    --with-push \
    --with-reader \
    --with-writer \
    --with-pattern \
    --with-sax1 \
    CC=$TOOLCHAIN_ROOT/bin/gcc \
    CXX=$TOOLCHAIN_ROOT/bin/g++

make -j"$(grep -c ^processor /proc/cpuinfo)" install

popd

# Transfer useful generated files
mkdir -p "$HOST_DIR/include/libxml"

# Copy the generated config header
cp "$HOST_DIR/build_tmp/config.h" "$HOST_DIR/include/"

# Copy the generated xmlversion.h
cp "$HOST_DIR/install_tmp/include/libxml2/libxml/xmlversion.h" "$HOST_DIR/include/libxml/"

# Clean up build artifacts
rm -rf "$HOST_DIR/build_tmp"
rm -rf "$HOST_DIR/install_tmp"

# Clean up dist directory - remove everything not needed for compilation
# Keep only: *.c, *.h source files, include/ directory, platform/ directory, LICENSE
pushd "$DEST_DIR/dist"

# Remove hidden files/directories
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;

# Remove directories not needed for compilation
rm -rf doc
rm -rf example
rm -rf fuzz
rm -rf python
rm -rf test
rm -rf result
rm -rf m4
rm -rf cmake
rm -rf os400
rm -rf win32
rm -rf vms
rm -rf macos
rm -rf *.cache

# Remove build system files (Makefiles, autoconf, automake, cmake, meson, etc.)
find . -type f -name "Makefile*" -delete
find . -type f -name "*.am" -delete
find . -type f -name "*.m4" -delete
find . -type f -name "*.cmake" -delete
find . -type f -name "CMakeLists.txt" -delete
rm -f configure configure.ac aclocal.m4
rm -f config.guess config.sub
rm -f autogen.sh ltmain.sh depcomp compile missing install-sh
rm -f test-driver ar-lib ylwrap py-compile
rm -f meson.build meson_options.txt

# Remove pkg-config and library config files
find . -type f -name "*.pc" -delete
find . -type f -name "*.pc.in" -delete
rm -f xml2-config.in xml2-config.1 libxml-2.0.pc.in libxml-2.0-uninstalled.pc.in
rm -f libxml2-config.cmake.in

# Remove spec files and packaging files
find . -type f -name "*.spec" -delete
find . -type f -name "*.spec.in" -delete
find . -type f -name "*.def" -delete
find . -type f -name "*.def.src" -delete
rm -f libxml2.syms

# Remove documentation files (keep only LICENSE/COPYING)
rm -f NEWS TODO AUTHORS INSTALL README README.zOS ChangeLog Copyright
rm -f HACKING MAINTAINERS
find . -type f -name "*.1" -delete  # man pages
find . -type f -name "*.html" -delete
find . -type f \( -name "*.md" -o -name "*.rst" -o -name "*.txt" \) ! -name "LICENSE*" ! -name "COPYING*" -delete

# Remove test and example source files
rm -f testapi.c testAutomata.c testC14N.c testchar.c testdict.c
rm -f testHTML.c testlimits.c testModule.c testOOM.c testparser.c
rm -f testReader.c testrecurse.c testRegexp.c testRelax.c testSAX.c
rm -f testSchemas.c testThreads.c testURI.c testXPath.c testXPathExpr.c
rm -f runtest.c runsuite.c runxmlconf.c
# Command-line tools (contain main() functions that conflict with test binaries)
rm -f xmllint.c xmlcatalog.c

# Remove Python binding files
find . -type f -name "*.py" -delete

# Remove shell scripts
find . -type f -name "*.sh" -delete
rm xml2-config-meson

# Remove editor backup files and other temporary files
find . -type f \( -name "*~" -o -name "*.bak" -o -name "*.orig" -o -name "*.rej" \) -delete

# Remove any generated object files or libraries (shouldn't exist but just in case)
find . -type f \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.dylib" -o -name "*.la" -o -name "*.lo" \) -delete

# Remove empty directories (except platform/)
find . -mindepth 1 -type d -empty ! -path "./platform*" -delete

popd

# Generate MODULE.bazel for bzlmod
cat > "$DEST_DIR/dist/MODULE.bazel" << 'EOF'
module(
    name = "libxml2",
    version = "2.15.1",
)

bazel_dep(name = "rules_cc", version = "0.0.9")
bazel_dep(name = "bazel_skylib", version = "1.7.1")
bazel_dep(name = "platforms", version = "0.0.9")
EOF

# Generate BUILD.bazel for the library
cat > "$DEST_DIR/dist/BUILD.bazel" << 'EOF'
# BUILD.bazel for vendored libxml2

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # MIT License

# Platform-specific include directories for generated headers
LIBXML2_PLATFORM_COPTS = select({
    "@platforms//cpu:aarch64": [
        "-Iexternal/libxml2/platform/linux_aarch64/include",
    ],
    "@platforms//cpu:x86_64": [
        "-Iexternal/libxml2/platform/linux_x86_64/include",
    ],
    "@platforms//cpu:s390x": [
        "-Iexternal/libxml2/platform/linux_s390x/include",
    ],
    "@platforms//cpu:ppc": [
        "-Iexternal/libxml2/platform/linux_ppc64le/include",
    ],
    "//conditions:default": [],
})

# Common compiler options
LIBXML2_COMMON_COPTS = [
    "-Wno-error",
    "-Wno-unused-parameter",
    "-Wno-missing-field-initializers",
    "-Wno-implicit-fallthrough",
]

cc_library(
    name = "libxml2",
    srcs = glob(
        [
            "*.c",
        ],
        exclude = [
            # Test/example files
            "test*.c",
            "run*.c",
            # Command-line tools (contain main() functions)
            "xmllint.c",
            "lintmain.c",
            "xmlcatalog.c",
            # Platform-specific files we don't need
            "trio*.c",
        ],
    ),
    hdrs = glob([
        "include/libxml/*.h",
        "include/private/*.h",
        "codegen/*.inc",
        "*.h",
    ]) + select({
        "@platforms//cpu:aarch64": glob([
            "platform/linux_aarch64/include/*.h",
            "platform/linux_aarch64/include/libxml/*.h",
        ]),
        "@platforms//cpu:x86_64": glob([
            "platform/linux_x86_64/include/*.h",
            "platform/linux_x86_64/include/libxml/*.h",
        ]),
        "@platforms//cpu:s390x": glob([
            "platform/linux_s390x/include/*.h",
            "platform/linux_s390x/include/libxml/*.h",
        ]),
        "@platforms//cpu:ppc": glob([
            "platform/linux_ppc64le/include/*.h",
            "platform/linux_ppc64le/include/libxml/*.h",
        ]),
        "//conditions:default": [],
    }),
    copts = LIBXML2_COMMON_COPTS + LIBXML2_PLATFORM_COPTS + [
        "-Iexternal/libxml2/include",
        "-Iexternal/libxml2",
    ],
    includes = [
        "include",
    ] + select({
        "@platforms//cpu:aarch64": [
            "platform/linux_aarch64/include",
        ],
        "@platforms//cpu:x86_64": [
            "platform/linux_x86_64/include",
        ],
        "@platforms//cpu:s390x": [
            "platform/linux_s390x/include",
        ],
        "@platforms//cpu:ppc": [
            "platform/linux_ppc64le/include",
        ],
        "//conditions:default": [],
    }),
    linkopts = select({
        "@platforms//os:linux": [
            "-lpthread",
            "-ldl",
        ],
        "//conditions:default": [],
    }),
    local_defines = [
        "HAVE_CONFIG_H",
        "_REENTRANT",
    ],
    target_compatible_with = select({
        "@platforms//os:linux": [],
        "//conditions:default": ["@platforms//:incompatible"],
    }),
)
EOF

echo "libxml2 import complete"


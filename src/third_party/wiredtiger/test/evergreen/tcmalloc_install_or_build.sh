#!/bin/bash
#
# Make a tcmalloc shared library available for preloading into the environment.

set -u

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <build-variant>"
    exit 1
fi

build_variant=$1

# This is tcmalloc upstream revision 093ba93 source patched by the SERVER team.
# https://github.com/mongodb-forks/tcmalloc/releases/tag/mongo-SERVER-85737
PATCHED_SRC=mongo-SERVER-85737
PATCHED_TGZ="${PATCHED_SRC}.tar.gz"
PATCHED_TGZ_URL="https://github.com/mongodb-forks/tcmalloc/archive/refs/tags/${PATCHED_TGZ}"
PATCHED_SRC_DIR="tcmalloc-${PATCHED_SRC}"

# Sensitive variables retrieved through Evergreen so they won't be exposed
# in source control.
S3_URL=${s3_bucket_tcmalloc}
# Export the AWS_* variables so that they are visible for the 'aws' cli.
export AWS_ACCESS_KEY_ID=${s3_access_key}
export AWS_SECRET_ACCESS_KEY=${s3_secret_key}

# Directory into which shared object will be installed.
install_dir=$PWD
tcmalloc_dir=TCMALLOC_LIB
tcmalloc_so_dir=${install_dir}/${tcmalloc_dir}

# Location in AWS S3 for prebuilt tcmalloc binaries.
PREBUILT_TGZ="tcmalloc-${PATCHED_SRC}-${build_variant}.tgz"
PREBUILT_URL="${S3_URL}/build/wt_prebuilt_tcmalloc/${PATCHED_SRC}/${PREBUILT_TGZ}"

# Without the --quiet option 'aws s3 cp' will print out an error message if the file is
# NOT present. This is an expected error case that is handled later in this script, but
# emitting messages with "Error" in them, into the Evergreen log will cause confusion.
echo "Attempting to download prebuilt tcmalloc: ${PREBUILT_URL}"
aws s3 cp --quiet ${PREBUILT_URL} ${PREBUILT_TGZ}
aws_ret=$?
if [[ $aws_ret -eq 0 ]]; then
    tar zxf $PREBUILT_TGZ
    exit $?
elif [[ $aws_ret -ne 1 ]]; then
    echo "ERROR aws s3 download failed with code ${aws_ret}"
    exit $aws_ret
fi

# Downloading a prebuilt copy failed. Assume it is because it doesn't exist for this build
# variant. Attempt to create a shared object for this build variant and upload it.
# From this point onward: any and all errors are fatal, and will cause the build to FAIL.
set -e

# Download and unpack. Allow a generous 2 minutes for the download to complete.
curl --retry 5 -L $PATCHED_TGZ_URL -sS --max-time 120 --fail --output ${PATCHED_TGZ}
rm -rf ${PATCHED_SRC_DIR}
tar zxf ${PATCHED_TGZ}

# Create this Bazel BUILD file in the top of the source directory to build a
# shared object, and then build.
cat << EOF > ${PATCHED_SRC_DIR}/BUILD
package(default_visibility = ["//visibility:private"])

cc_shared_library(
    name = "libtcmalloc",
    deps = [
     "//tcmalloc:tcmalloc",
    ],
    shared_lib_name = "libtcmalloc.so",
    visibility = ["//visibility:public"],
)
EOF

(cd $PATCHED_SRC_DIR ;

 # FIXME-WT-12775 When DEVPROD upgrades to using Bazel 7+, and the corresponding
 # Evergreen sandboxing is fixed. The bazel invocation should be reviewed and
 # hopefully simplified.
 mkdir -p /data/tmp/bazel-working-directory/_main ;
 PATH=/opt/mongodbtoolchain/v4/bin:$PATH bazel build --sandbox_debug --sandbox_writable_path=/data/tmp/bazel-working-directory --sandbox_writable_path=/data/tmp/bazel-working-directory/_main --verbose_failures libtcmalloc )

# Package and upload. If the upload fails: fail the WT build, even though
# there is an available binary. This is to ensure any problem becomes
# quickly visible in Evergreen.
rm -rf $tcmalloc_so_dir
mkdir -p $tcmalloc_so_dir
cp ${PATCHED_SRC_DIR}/bazel-bin/libtcmalloc.so $tcmalloc_so_dir
tar zcf $PREBUILT_TGZ $tcmalloc_dir
aws s3 cp $PREBUILT_TGZ $PREBUILT_URL
echo "Uploaded new prebuilt tcmalloc: ${PREBUILT_URL}"

# Build and upload of tcmalloc was successful. Now use the locally
# built copy of tcmalloc for the WT build.
exit 0

# Verifies that the compile_variant matches build_variant.
# When a compile is done as part of a build variant, the compile_variant needs to match
# the build_variant name to ensure that the output S3 folders for the compile are correct.
# This is because compile_variant is used to actually determine the output folders for
# artifacts, and checking that the compile variant actually matches the build variant avoids
# naming conflicts that can lead to hard to debug race conditions.

set -euo pipefail

if [ -z "${build_variant}" ]; then
    echo "ERROR: build_variant is not set"
    exit 1
fi

# These variants are exempted because they historically used a different compile_variant name
exempted_variants=(
    "enterprise-rhel-81-ppc64le-dynamic"
    "enterprise-rhel-83-s390x-dynamic"
    "enterprise-rhel-9-ppc64le-dynamic"
    "enterprise-rhel-9-s390x-dynamic"
    "rhel8"
)
for exempted in "${exempted_variants[@]}"; do
    if [ "${build_variant}" == "$exempted" ]; then
        echo "Skipping compile_variant check for exempted variant: ${build_variant}"
        exit 0
    fi
done
if [ "${compile_variant}" != "${build_variant}" ]; then
    echo "ERROR: compile_variant '${compile_variant}' does not match build_variant '${build_variant}'"
    exit 1
fi

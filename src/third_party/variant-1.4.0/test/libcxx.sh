# MPark.Variant
#
# Copyright Michael Park, 2015-2017
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#!/usr/bin/env bash

set -e

trap "cd ${MPARK_VARIANT_LIBCXX_SOURCE_DIR} && git checkout ." EXIT

cat <<EOF > ${MPARK_VARIANT_LIBCXX_SOURCE_DIR}/include/variant
#define mpark std
#define MPARK_IN_PLACE_HPP
$(cat ${MPARK_VARIANT_SOURCE_DIR}/include/mpark/variant.hpp)
#undef MPARK_IN_PLACE_HPP
#undef mpark
EOF

${MPARK_VARIANT_LIT} \
    -v \
    --param color_diagnostics \
    --param cxx_under_test="${MPARK_VARIANT_CXX_COMPILER}" \
    --param compile_flags=-I${MPARK_VARIANT_SOURCE_DIR}/include/mpark \
    --param libcxx_site_config=${MPARK_VARIANT_LIBCXX_SITE_CONFIG} \
    --param std=c++17 \
    --param use_clang_verify=false \
    ${MPARK_VARIANT_LIBCXX_SOURCE_DIR}/test/std/utilities/variant \

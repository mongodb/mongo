/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * MSVC wants us to use the non-portable _dupenv_s instead; since we need
 * to remain portable, tell MSVC to suppress this warning.
 */

#include <aws/common/cpuid.h>

bool aws_cpu_has_feature(enum aws_cpu_feature_name feature_name) {
    (void)feature_name;
    return false;
}

/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/identity/auth/AuthSchemeOption.h>

namespace smithy {
    struct SigV4AuthSchemeOption
    {
        static SMITHY_API AuthSchemeOption sigV4AuthSchemeOption;
    };
}
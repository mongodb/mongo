/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthSchemeOption.h>
namespace smithy
{
struct BearerTokenAuthSchemeOption
{
    static AuthSchemeOption bearerTokenAuthSchemeOption;
};

AuthSchemeOption BearerTokenAuthSchemeOption::bearerTokenAuthSchemeOption =
    AuthSchemeOption("smithy.api#HTTPBearerAuth");
} // namespace smithy
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <array>

namespace Aws
{
    template< typename T, std::size_t N > using Array = std::array< T, N >;
} // namespace Aws

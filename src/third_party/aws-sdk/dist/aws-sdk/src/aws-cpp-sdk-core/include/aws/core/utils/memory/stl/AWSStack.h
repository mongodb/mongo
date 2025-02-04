/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/memory/stl/AWSDeque.h>

#include <deque>
#include <stack>

namespace Aws
{

template< typename T > using Stack = std::stack< T, Deque< T > >;

} // namespace Aws

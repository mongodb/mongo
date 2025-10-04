/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSAllocator.h>

#include <map>

namespace Aws
{

template< typename K, typename V > using MultiMap = std::multimap< K, V, std::less< K >, Aws::Allocator< std::pair< const K, V > > >;

} // namespace Aws

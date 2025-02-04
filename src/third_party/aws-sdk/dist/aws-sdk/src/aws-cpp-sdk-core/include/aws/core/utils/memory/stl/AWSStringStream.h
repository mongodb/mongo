/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#if defined(_GLIBCXX_FULLY_DYNAMIC_STRING) && _GLIBCXX_FULLY_DYNAMIC_STRING == 0 && defined(__ANDROID__)

#include <aws/core/utils/memory/stl/SimpleStringStream.h>

#else

#include <aws/core/utils/memory/stl/AWSAllocator.h>

#include <sstream>

#endif

namespace Aws
{

#if defined(_GLIBCXX_FULLY_DYNAMIC_STRING) && _GLIBCXX_FULLY_DYNAMIC_STRING == 0 && defined(__ANDROID__)

// see the large comment block in AWSString.h  for an explanation
typedef Aws::SimpleStringStream StringStream;
typedef Aws::SimpleIStringStream IStringStream;
typedef Aws::SimpleOStringStream OStringStream;
typedef Aws::Utils::Stream::SimpleStreamBuf StringBuf;

#else

typedef std::basic_stringstream< char, std::char_traits< char >, Aws::Allocator< char > > StringStream;
typedef std::basic_istringstream< char, std::char_traits< char >, Aws::Allocator< char > > IStringStream;
typedef std::basic_ostringstream< char, std::char_traits< char >, Aws::Allocator< char > > OStringStream;
typedef std::basic_stringbuf< char, std::char_traits< char >, Aws::Allocator< char > > StringBuf;

#endif

} // namespace Aws

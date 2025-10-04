/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <iostream>
#include <functional>

namespace Aws
{

// Serves no purpose other than to help my conversion process
typedef std::basic_ifstream< char, std::char_traits< char > > IFStream;
typedef std::basic_ofstream< char, std::char_traits< char > > OFStream;
typedef std::basic_fstream< char, std::char_traits< char > > FStream;
typedef std::basic_istream< char, std::char_traits< char > > IStream;
typedef std::basic_ostream< char, std::char_traits< char > > OStream;
typedef std::basic_iostream< char, std::char_traits< char > > IOStream;
typedef std::istreambuf_iterator< char, std::char_traits< char > > IStreamBufIterator;

using IOStreamFactory = std::function< Aws::IOStream*(void) >;


} // namespace Aws

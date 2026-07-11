// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include <map>
#include <string>
#include <string_view>

namespace mongo::transport::grpc {

/**
 * A gRPC metadata map that owns its keys and values.
 */
using MetadataContainer = std::multimap<std::string, std::string>;

/**
 * A gRPC metadata map that references its keys and values but does not own them.
 */
using MetadataView = std::multimap<std::string_view, std::string_view>;

}  // namespace mongo::transport::grpc

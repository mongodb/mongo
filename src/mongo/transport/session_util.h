// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/net/cidr.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo::transport::util {
bool isExemptedByCIDRList(const SockAddr& ra, const SockAddr& la, const CIDRList& exemptions);
}  // namespace mongo::transport::util

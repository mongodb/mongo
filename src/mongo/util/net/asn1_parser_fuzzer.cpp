// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/base/data_range.h"
#include "mongo/util/net/ssl_manager.h"

#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

void PeerRoleParse(std::vector<char> Data) {
    mongo::ConstDataRange dr = mongo::ConstDataRange(Data);
    auto ret = mongo::parsePeerRoles(dr);
}
FUZZ_TEST(ASN1FuzzSuite, PeerRoleParse);

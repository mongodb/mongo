// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/sbe_unittest.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <iostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::sbe {

unittest::GoldenTestConfig goldenTestConfigSbe{"src/mongo/db/test_output/exec/sbe"};

void GoldenSBETestFixture::setUp() {
    gctx = std::make_unique<unittest::GoldenTestContext>(&goldenTestConfigSbe);
    gctx->validateOnClose(false);
    gctx->printTestHeader(unittest::GoldenTestContext::HeaderFormat::Text);
}

void GoldenSBETestFixture::tearDown() {
    gctx->verifyOutput();
    gctx.reset();
}

void GoldenSBETestFixture::printVariation(const std::string& name) {
    auto& os = gctx->outStream();
    _variationCount++;
    os << "==== VARIATION ";
    if (!name.empty()) {
        os << name;
    } else {
        os << _variationCount;
    }
    os << std::endl;
}
}  // namespace mongo::sbe

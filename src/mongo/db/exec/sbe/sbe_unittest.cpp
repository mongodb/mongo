/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/sbe/sbe_unittest.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <iostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::sbe {

unittest::GoldenTestConfig goldenTestConfigSbe{"src/mongo/db/test_output/exec/sbe"};

void GoldenSBETestFixture::run() {
    GoldenTestContext ctx(&goldenTestConfigSbe);
    ctx.validateOnClose(false);
    auto guard = ScopeGuard([&] { gctx = nullptr; });
    gctx = &ctx;
    gctx->printTestHeader(GoldenTestContext::HeaderFormat::Text);

    if (_debug) {
        try {
            Test::run();
        } catch (::mongo::unittest::TestAssertionFailureException&) {
            std::cout << "== Golden test failed before output comparison. ==" << std::endl;
            std::cout << "Output so far:" << std::endl << ctx.getOutputString() << std::endl;
            std::cout.flush();
            throw;
        }
    } else {
        Test::run();
    }

    ctx.verifyOutput();
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

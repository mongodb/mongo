/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/intrusive_ptr.hpp>

namespace mongo {

/**
 * Test fixture which provides an ExpressionContext for use in testing along with providing access
 * to a storage engine for testing.
 */
class AggregationMongoDContextFixture : public ServiceContextMongoDTest {
public:
    AggregationMongoDContextFixture()
        : AggregationMongoDContextFixture(
              NamespaceString::createNamespaceString_forTest("unittests.pipeline_test")) {}

    AggregationMongoDContextFixture(NamespaceString nss)
        : _expCtx(new ExpressionContextForTest(_opCtx.get(), nss)) {
        unittest::TempDir tempDir("AggregationMongoDContextFixture");
        _expCtx->setTempDir(tempDir.path());
    }

    auto getExpCtx() {
        return _expCtx;
    }

    auto getExpCtxRaw() {
        return _expCtx.get();
    }

    auto getOpCtx() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx = makeOperationContext();
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};
}  // namespace mongo

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <memory>
#include <vector>

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

/**
 * Test fixture which provides an ExpressionContext for use in testing.
 */
class AggregationContextFixture : public ServiceContextTest {
public:
    AggregationContextFixture()
        : AggregationContextFixture(NamespaceString::createNamespaceString_forTest(
              boost::none, "unittests", "pipeline_test")) {}

    AggregationContextFixture(NamespaceString nss) {
        auto service = getServiceContext();
        service->registerClientObserver(
            std::make_unique<LockerNoopClientObserverWithReplacementPolicy>());
        _opCtx = makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>(_opCtx.get(), nss);
        unittest::TempDir tempDir("AggregationContextFixture");
        _expCtx->tempDir = tempDir.path();
        _expCtx->changeStreamSpec = DocumentSourceChangeStreamSpec();
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

    /*
     * Serialize and redact a document source.
     */
    BSONObj redact(const DocumentSource& docSource, bool performRedaction = true) {
        SerializationOptions options;
        if (performRedaction) {
            options.replacementForLiteralArgs = "?";
            options.identifierRedactionPolicy = [](StringData s) -> std::string {
                return str::stream() << "HASH<" << s << ">";
            };
            options.redactIdentifiers = true;
        }
        std::vector<Value> serialized;
        docSource.serializeToArray(serialized, options);
        ASSERT_EQ(1, serialized.size());
        return serialized[0].getDocument().toBson().getOwned();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

// A custom-deleter which disposes a DocumentSource when it goes out of scope.
struct DocumentSourceDeleter {
    void operator()(DocumentSource* docSource) {
        docSource->dispose();
        delete docSource;
    }
};

class ServerlessAggregationContextFixture : public AggregationContextFixture {
public:
    ServerlessAggregationContextFixture()
        : AggregationContextFixture(NamespaceString::createNamespaceString_forTest(
              TenantId(OID::gen()), "unittests", "pipeline_test")) {}

    const std::string _targetDb = "test";
    const std::string _targetColl = "target_collection";
};

}  // namespace mongo

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
    struct ExpressionContextOptionsStruct {
        bool inMongos = false;
        bool requiresTimeseriesExtendedRangeSupport = false;
    };

    AggregationContextFixture()
        : AggregationContextFixture(NamespaceString::createNamespaceString_forTest(
              boost::none, "test", "pipeline_test")) {}

    AggregationContextFixture(NamespaceString nss) {
        _opCtx = makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>(_opCtx.get(), nss);
        _expCtx->tempDir = _tempDir.path();
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

    void setExpCtx(ExpressionContextOptionsStruct options) {
        _expCtx->inMongos = options.inMongos;
        _expCtx->setRequiresTimeseriesExtendedRangeSupport(
            options.requiresTimeseriesExtendedRangeSupport);
    }

    /*
     * Serialize and redact a document source.
     */
    BSONObj redact(const DocumentSource& docSource,
                   bool performRedaction = true,
                   boost::optional<ExplainOptions::Verbosity> verbosity = boost::none) {
        SerializationOptions options;
        options.verbosity = verbosity;
        if (performRedaction) {
            options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
            options.transformIdentifiersCallback = [](StringData s) -> std::string {
                return str::stream() << "HASH<" << s << ">";
            };
            options.transformIdentifiers = true;
        }
        std::vector<Value> serialized;
        docSource.serializeToArray(serialized, options);
        ASSERT_EQ(1, serialized.size());
        return serialized[0].getDocument().toBson().getOwned();
    }

    std::vector<Value> redactToArray(const DocumentSource& docSource,
                                     bool performRedaction = true) {
        SerializationOptions options;
        if (performRedaction) {
            options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
            options.transformIdentifiersCallback = [](StringData s) -> std::string {
                return str::stream() << "HASH<" << s << ">";
            };
            options.transformIdentifiers = true;
        }
        std::vector<Value> serialized;
        docSource.serializeToArray(serialized, options);
        return serialized;
    }

    void makePipelineOptimizeAssertNoRewrites(
        boost::intrusive_ptr<mongo::ExpressionContextForTest> expCtx,
        std::vector<BSONObj> expectedStages) {
        auto optimizedPipeline = Pipeline::parse(expectedStages, expCtx);
        optimizedPipeline->optimizePipeline();
        auto optimizedSerialized = optimizedPipeline->serializeToBson();

        ASSERT_EQ(expectedStages.size(), optimizedSerialized.size());
        for (size_t i = 0; i < expectedStages.size(); i++) {
            ASSERT_BSONOBJ_EQ(expectedStages[i], optimizedSerialized[i]);
        }
    }

private:
    const unittest::TempDir _tempDir{"AggregationContextFixture"};

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

namespace {
// A utility function to convert pipeline (a vector of BSONObj) to a string. Helpful for debugging.
std::string to_string(const std::vector<BSONObj>& objs) {
    std::stringstream sstrm;
    sstrm << "[" << std::endl;
    for (const auto& obj : objs) {
        sstrm << obj.toString() << "," << std::endl;
    }
    sstrm << "]" << std::endl;
    return sstrm.str();
}
}  // namespace
}  // namespace mongo

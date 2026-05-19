/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 */

#include "mongo/db/modules/enterprise/src/streams/exec/embed/planning.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/modules/enterprise/src/streams/exec/embed/embed_operator.h"
#include "mongo/db/modules/enterprise/src/streams/exec/tests/operator_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

namespace mongo::streams::embed {
namespace {

class EmbedPlanningTest : public OperatorTestFixture {
protected:
    void registerVoyageConnection() {
        ctx()->connectionRegistry()->registerForTest("voyage-ai",
                                                     ConnectionRecord{"voyage",
                                                                      "https://api.voyageai.com",
                                                                      "secret-key",
                                                                      "",
                                                                      ""});
    }

    EmbedStageSpec parse(BSONObj spec) {
        return EmbedStageSpec::parse(IDLParserContext{"$embed"}, spec);
    }
};

TEST_F(EmbedPlanningTest, MinimalSpecBuilds) {
    registerVoyageConnection();
    auto spec = parse(BSON("input"
                           << "$body"
                           << "into"
                           << "embedding"
                           << "model"
                           << BSON("connectionName"
                                   << "voyage-ai"
                                   << "name"
                                   << "voyage-3-large")));
    auto op = makeEmbedOperator(ctx(), spec);
    ASSERT(op != nullptr);
    ASSERT_EQ("embed", op->kind());
}

TEST_F(EmbedPlanningTest, MissingConnectionFails) {
    auto spec = parse(BSON("input"
                           << "$body"
                           << "into"
                           << "embedding"
                           << "model"
                           << BSON("connectionName"
                                   << "does-not-exist"
                                   << "name"
                                   << "voyage-3-large")));
    ASSERT_THROWS_CODE(makeEmbedOperator(ctx(), spec), DBException, ErrorCodes::BadValue);
}

TEST_F(EmbedPlanningTest, UnsupportedConnectionTypeRejected) {
    ctx()->connectionRegistry()->registerForTest(
        "weird",
        ConnectionRecord{"some-unsupported-type", "https://x", "k", "", ""});
    auto spec = parse(BSON("input"
                           << "$body"
                           << "into"
                           << "v"
                           << "model"
                           << BSON("connectionName"
                                   << "weird"
                                   << "name"
                                   << "m")));
    ASSERT_THROWS_CODE(makeEmbedOperator(ctx(), spec), DBException, ErrorCodes::BadValue);
}

TEST_F(EmbedPlanningTest, ZeroDimensionsRejected) {
    registerVoyageConnection();
    auto spec = parse(BSON("input"
                           << "$body"
                           << "into"
                           << "v"
                           << "model"
                           << BSON("connectionName"
                                   << "voyage-ai"
                                   << "name"
                                   << "voyage-3-large"
                                   << "dimensions" << 0)));
    ASSERT_THROWS_CODE(makeEmbedOperator(ctx(), spec), DBException, ErrorCodes::BadValue);
}

TEST_F(EmbedPlanningTest, BatchAndCacheDefaultsApplied) {
    registerVoyageConnection();
    auto spec = parse(BSON("input"
                           << "$body"
                           << "into"
                           << "v"
                           << "model"
                           << BSON("connectionName"
                                   << "voyage-ai"
                                   << "name"
                                   << "m")));
    // Defaults: maxSize=96, maxWaitMs=50, cache off. These are checked via the
    // IDL-defaulted accessor values; we just round-trip and ensure parsing
    // accepts the omission.
    ASSERT_EQ(96, EmbedBatchSpec{}.getMaxSize());
    ASSERT_EQ(50, EmbedBatchSpec{}.getMaxWaitMs());
    ASSERT_FALSE(EmbedCacheSpec{}.getEnabled());
    auto op = makeEmbedOperator(ctx(), spec);
    ASSERT(op != nullptr);
}

TEST_F(EmbedPlanningTest, ComplexInputExpressionAccepted) {
    registerVoyageConnection();
    auto spec = parse(fromjson(R"({
      input: { $concat: ["$subject", " ", "$body"] },
      into: "embedding",
      model: { connectionName: "voyage-ai", name: "voyage-3-large" },
      batch: { maxSize: 32, maxWaitMs: 200 },
      cache: { enabled: true, maxEntries: 500 },
      onError: "dlq"
    })"));
    auto op = makeEmbedOperator(ctx(), spec);
    ASSERT(op != nullptr);
}

TEST_F(EmbedPlanningTest, EmptyIntoRejected) {
    registerVoyageConnection();
    auto spec = parse(BSON("input"
                           << "$body"
                           << "into"
                           << ""
                           << "model"
                           << BSON("connectionName"
                                   << "voyage-ai"
                                   << "name"
                                   << "m")));
    ASSERT_THROWS(makeEmbedOperator(ctx(), spec), DBException);
}

}  // namespace
}  // namespace mongo::streams::embed

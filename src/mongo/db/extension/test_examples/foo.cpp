/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_status.h"

namespace sdk = mongo::extension::sdk;

class TestFooLogicalStage : public mongo::extension::sdk::LogicalAggregationStage {};

class TestFooStageDescriptor : public mongo::extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$testFoo";

    TestFooStageDescriptor()
        : mongo::extension::sdk::AggregationStageDescriptor(
              kStageName, MongoExtensionAggregationStageType::kNoOp) {}

    std::unique_ptr<mongo::extension::sdk::LogicalAggregationStage> parse(
        mongo::BSONObj stageBson) const override {
        uassert(10624200,
                "Failed to parse " + kStageName + ", expected object",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());

        return std::make_unique<TestFooLogicalStage>();
    }
};


MongoExtensionStatus* initialize_extension(MongoExtensionHostPortal* portal) {
    return sdk::enterCXX([&]() {
        static sdk::ExtensionAggregationStageDescriptor testFooDescriptor{
            std::make_unique<TestFooStageDescriptor>()};
        return sdk::enterC([&]() {
            return portal->registerStageDescriptor(
                reinterpret_cast<const ::MongoExtensionAggregationStageDescriptor*>(
                    &testFooDescriptor));
        });
    });
}

static const MongoExtension my_extension = {
    .version = MONGODB_EXTENSION_API_VERSION,
    .initialize = initialize_extension,
};

extern "C" {
MongoExtensionStatus* get_mongodb_extension(const MongoExtensionAPIVersionVector* hostVersions,
                                            const MongoExtension** extension) {
    return mongo::extension::sdk::enterCXX([&]() { *extension = &my_extension; });
}
}

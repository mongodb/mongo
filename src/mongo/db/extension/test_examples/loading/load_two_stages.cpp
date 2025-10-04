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
#include "mongo/db/extension/sdk/extension_factory.h"

namespace sdk = mongo::extension::sdk;

class FooLogicalStage : public mongo::extension::sdk::LogicalAggregationStage {};

class FooStageDescriptor : public mongo::extension::sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$foo";

    FooStageDescriptor()
        : mongo::extension::sdk::AggregationStageDescriptor(
              kStageName, MongoExtensionAggregationStageType::kNoOp) {}

    std::unique_ptr<mongo::extension::sdk::LogicalAggregationStage> parse(
        mongo::BSONObj stageBson) const override {
        uassert(10696400,
                "Failed to parse " + kStageName + ", expected object",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());

        return std::make_unique<FooLogicalStage>();
    }
};

class BarLogicalStage : public mongo::extension::sdk::LogicalAggregationStage {};

class BarStageDescriptor : public sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$bar";

    BarStageDescriptor()
        : sdk::AggregationStageDescriptor(kStageName, MongoExtensionAggregationStageType::kNoOp) {}

    std::unique_ptr<sdk::LogicalAggregationStage> parse(mongo::BSONObj stageBson) const override {
        uassert(10696401,
                "Failed to parse " + kStageName + ", expected object",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());

        return std::make_unique<BarLogicalStage>();
    }
};

class MyExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<FooStageDescriptor>(portal);
        _registerStage<BarStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MyExtension)
DEFINE_GET_EXTENSION()

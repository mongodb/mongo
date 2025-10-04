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

/**
 * $testBar is a no-op stage.
 *
 * The stage definition must NOT be empty or it will fail to parse. The contents of the stage
 * definition can be anything, as long as it is not an empty object.
 */
class TestBarLogicalStage : public sdk::LogicalAggregationStage {};

class TestBarStageDescriptor : public sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$testBar";

    TestBarStageDescriptor()
        : sdk::AggregationStageDescriptor(kStageName, MongoExtensionAggregationStageType::kNoOp) {}

    std::unique_ptr<sdk::LogicalAggregationStage> parse(mongo::BSONObj stageBson) const override {
        uassert(10845401,
                "Failed to parse " + kStageName + ", expected object",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());

        uassert(10785800,
                "Failed to parse " + kStageName + ", must have at least one field",
                !stageBson.getField(kStageName).Obj().isEmpty());

        return std::make_unique<TestBarLogicalStage>();
    }
};

class BarExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<TestBarStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(BarExtension)
DEFINE_GET_EXTENSION()

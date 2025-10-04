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

class TestFooForHostServicesAsapLogicalStage : public sdk::LogicalAggregationStage {};

class TestFooForHostServicesAsapStageDescriptor : public sdk::AggregationStageDescriptor {
public:
    static inline const std::string kStageName = "$testFooForHostServicesAsap";

    TestFooForHostServicesAsapStageDescriptor()
        : sdk::AggregationStageDescriptor(kStageName, MongoExtensionAggregationStageType::kNoOp) {}

    std::unique_ptr<sdk::LogicalAggregationStage> parse(mongo::BSONObj stageBson) const override {
        uassert(11097602,
                "Failed to parse " + kStageName + ", expected object",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj() &&
                    stageBson.getField(kStageName).Obj().isEmpty());

        uassert(11097603,
                "Dummy assertion to test usage of the host services",
                sdk::HostServicesHandle::getHostServices()->alwaysTrue_TEMPORARY());

        return std::make_unique<TestFooForHostServicesAsapLogicalStage>();
    }
};

class FooExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        // We test that the host services are accessible as soon as the initialization function is
        // invoked. This is the first entrypoint where extensions are allowed to call into the host
        // services.
        uassert(11097601,
                "Dummy assertion to test usage of the host services",
                sdk::HostServicesHandle::getHostServices()->alwaysTrue_TEMPORARY());

        _registerStage<TestFooForHostServicesAsapStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(FooExtension)
DEFINE_GET_EXTENSION()

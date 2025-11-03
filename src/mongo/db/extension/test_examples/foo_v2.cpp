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
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

namespace sdk = mongo::extension::sdk;

DEFAULT_LOGICAL_AST_PARSE(TestFoo, "$testFoo")

/**
 * $testFoo is a no-op stage.
 *
 * This file is identical to foo.cpp except this stage does _not_ fail parsing if the
 * stage definition is empty. This is used for extension upgrade/downgrade testing.
 */
class TestFooStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(TestFooStageName);

    TestFooStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        // Unlike foo.cpp, this will NOT fail to parse if the stage definition is not empty. Any/all
        // fields are just quietly ignored.
        sdk::validateStageDefinition(stageBson, kStageName);

        return std::make_unique<TestFooParseNode>(stageBson);
    }
};

class FooExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<TestFooStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(FooExtension)
DEFINE_GET_EXTENSION()

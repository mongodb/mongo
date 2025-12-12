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
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static const std::string kMatchTopNName = "$matchTopN";
static const std::string kMatchName = "$match";
static const std::string kSortName = "$sort";
static const std::string kLimitName = "$limit";

class MatchTopNParseNode : public sdk::AggStageParseNode {
public:
    // Input: {$matchTopN: {filter: {...}, sort: {...}, limit: <num>}}
    MatchTopNParseNode(BSONObj topN)
        : sdk::AggStageParseNode(kMatchTopNName),
          _input(topN.getOwned()),
          _matchSpec(BSON(kMatchName << _input["filter"].Obj())),
          _sortSpec(BSON(kSortName << _input["sort"].Obj())),
          _limitSpec(BSON(kLimitName << _input["limit"].numberInt())) {}

    size_t getExpandedSize() const override {
        return 3;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> out;
        out.reserve(getExpandedSize());

        // Expands to three stages:
        // 1) Host $match
        // 2) Host $sort
        // 3) Host $limit (TODO SERVER-114847 this should be an extension $limit once transform
        //    stages are implemented)
        auto* hostServices = sdk::HostServicesHandle::getHostServices();
        out.emplace_back(hostServices->createHostAggStageParseNode(_matchSpec));
        out.emplace_back(hostServices->createHostAggStageParseNode(_sortSpec));
        out.emplace_back(hostServices->createHostAggStageParseNode(_limitSpec));

        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSON(kMatchTopNName << _input);
    }

private:
    const BSONObj _input;      // {filter: {...}, sort: {...}, limit: <num>}
    const BSONObj _matchSpec;  // {$match: {...}}
    const BSONObj _sortSpec;   // {$sort: {...}}
    const BSONObj _limitSpec;  // {$limit: <int>}
};

class MatchTopNStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$matchTopN";

    MatchTopNStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        const auto obj = sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(10956500,
                    "$matchTopN requires 'filter' object",
                    obj.hasField("filter") && obj["filter"].isABSONObj());
        sdk_uassert(10956501,
                    "$matchTopN requires 'sort' object",
                    obj.hasField("sort") && obj["sort"].isABSONObj());

        const auto limitElem = obj["limit"];
        sdk_uassert(10956502, "$matchTopN requires a 'limit' field", !limitElem.eoo());
        const auto swLimit = limitElem.parseIntegerElementToNonNegativeLong();
        sdk_uassert(10956515,
                    "$matchTopN requires a positive integral 'limit'",
                    swLimit.isOK() && swLimit.getValue() >= 1);

        return std::make_unique<MatchTopNParseNode>(obj.getOwned());
    }
};

DEFAULT_EXTENSION(MatchTopN)
REGISTER_EXTENSION(MatchTopNExtension)
DEFINE_GET_EXTENSION()

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/aggregated_index_usage_tracker.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {
// List of index feature description strings.
static const std::string k2d = "2d";
static const std::string k2dSphere = "2dsphere";
static const std::string k2dSphereBucket = "2dsphere_bucket";
static const std::string kCollation = "collation";
static const std::string kCompound = "compound";
static const std::string kHashed = "hashed";
static const std::string kId = "id";
static const std::string kNormal = "normal";
static const std::string kPartial = "partial";
static const std::string kSingle = "single";
static const std::string kSparse = "sparse";
static const std::string kText = "text";
static const std::string kTTL = "ttl";
static const std::string kUnique = "unique";
static const std::string kWildcard = "wildcard";
static const std::string kColumn = "columnstore";

std::map<std::string, IndexFeatureStats> makeFeatureMap() {
    std::map<std::string, IndexFeatureStats> map;
    map[k2d];
    map[k2dSphere];
    map[k2dSphereBucket];
    map[kCollation];
    map[kCompound];
    map[kHashed];
    map[kId];
    map[kNormal];
    map[kPartial];
    map[kSingle];
    map[kSparse];
    map[kText];
    map[kTTL];
    map[kUnique];
    map[kWildcard];
    map[kColumn];
    return map;
};

const auto getAggregatedIndexUsageTracker =
    ServiceContext::declareDecoration<AggregatedIndexUsageTracker>();
}  // namespace


IndexFeatures IndexFeatures::make(const IndexDescriptor* desc, bool internal) {
    IndexFeatures features;

    int count = 0;
    for (auto it = desc->keyPattern().begin(); it != desc->keyPattern().end(); it++) {
        count++;
    }
    tassert(6325400, "index key pattern must have at least one element", count);

    auto indexType = IndexNames::nameToType(desc->getAccessMethodName());
    features.collation = !desc->collation().isEmpty();
    // Text indexes add an extra field internally, but we don't want to expose that detail.
    features.compound = (indexType == IndexType::INDEX_TEXT) ? count > 2 : count > 1;
    features.id = desc->isIdIndex();
    features.internal = internal;
    features.partial = desc->isPartial();
    features.sparse = desc->isSparse();
    features.ttl = desc->infoObj().hasField(IndexDescriptor::kExpireAfterSecondsFieldName);
    features.type = indexType;
    features.unique = desc->unique();
    return features;
}

AggregatedIndexUsageTracker::AggregatedIndexUsageTracker()
    : _indexFeatureToStats(makeFeatureMap()) {}

AggregatedIndexUsageTracker* AggregatedIndexUsageTracker::get(ServiceContext* svcCtx) {
    return &getAggregatedIndexUsageTracker(svcCtx);
}

void AggregatedIndexUsageTracker::onAccess(const IndexFeatures& features) const {
    if (!features.internal) {
        _updateStatsForEachFeature(features, [](auto stats) { stats->accesses.fetchAndAdd(1); });
    }
}

void AggregatedIndexUsageTracker::onRegister(const IndexFeatures& features) const {
    if (!features.internal) {
        _updateStatsForEachFeature(features, [](auto stats) { stats->count.fetchAndAdd(1); });
        _count.fetchAndAdd(1);
    }
}

void AggregatedIndexUsageTracker::onUnregister(const IndexFeatures& features) const {
    if (!features.internal) {
        _updateStatsForEachFeature(features, [](auto stats) { stats->count.fetchAndAdd(-1); });
        _count.fetchAndAdd(-1);
    }
}

void AggregatedIndexUsageTracker::_updateStatsForEachFeature(const IndexFeatures& features,
                                                             UpdateFn&& update) const {
    // Aggregate _id indexes separately so they do not get included with the other features.
    if (features.id) {
        update(&_indexFeatureToStats.at(kId));
        return;
    }

    switch (features.type) {
        case INDEX_BTREE:
            update(&_indexFeatureToStats.at(kNormal));
            break;
        case INDEX_2D:
            update(&_indexFeatureToStats.at(k2d));
            break;
        case INDEX_HAYSTACK:
            // MongoDB does not support haystack indexes. This enum only exists to reject creating
            // them.
            break;
        case INDEX_2DSPHERE:
            update(&_indexFeatureToStats.at(k2dSphere));
            break;
        case INDEX_2DSPHERE_BUCKET:
            update(&_indexFeatureToStats.at(k2dSphereBucket));
            break;
        case INDEX_TEXT:
            update(&_indexFeatureToStats.at(kText));
            break;
        case INDEX_HASHED:
            update(&_indexFeatureToStats.at(kHashed));
            break;
        case INDEX_WILDCARD:
            update(&_indexFeatureToStats.at(kWildcard));
            break;
        case INDEX_COLUMN:
            update(&_indexFeatureToStats.at(kColumn));
            break;
        default:
            MONGO_UNREACHABLE;
    }

    if (features.collation) {
        update(&_indexFeatureToStats.at(kCollation));
    }
    if (features.compound) {
        update(&_indexFeatureToStats.at(kCompound));
    } else {
        update(&_indexFeatureToStats.at(kSingle));
    }
    if (features.partial) {
        update(&_indexFeatureToStats.at(kPartial));
    }
    if (features.sparse) {
        update(&_indexFeatureToStats.at(kSparse));
    }
    if (features.ttl) {
        update(&_indexFeatureToStats.at(kTTL));
    }
    if (features.unique) {
        update(&_indexFeatureToStats.at(kUnique));
    }
}

void AggregatedIndexUsageTracker::forEachFeature(OnFeatureFn&& onFeature) const {
    for (auto& [key, value] : _indexFeatureToStats) {
        onFeature(key, value);
    }
}

long long AggregatedIndexUsageTracker::getCount() const {
    return _count.load();
}

class IndexStatsSSS : public ServerStatusSection {
public:
    IndexStatsSSS() : ServerStatusSection("indexStats") {}

    ~IndexStatsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto globalFeatures = AggregatedIndexUsageTracker::get(opCtx->getServiceContext());

        BSONObjBuilder builder;
        builder.append("count", globalFeatures->getCount());

        BSONObjBuilder featuresBuilder = builder.subobjStart("features");
        globalFeatures->forEachFeature(
            [&featuresBuilder](const std::string& feature, const IndexFeatureStats& stats) {
                BSONObjBuilder featureBuilder = featuresBuilder.subobjStart(feature);
                featureBuilder.append("count", stats.count.load());
                featureBuilder.append("accesses", stats.accesses.load());
                featureBuilder.done();
            });
        featuresBuilder.done();
        return builder.obj();
    }
} indexStatsSSS;
}  // namespace mongo

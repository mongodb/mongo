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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

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
static const std::string kPrepareUnique = "prepareUnique";
static const std::string kSingle = "single";
static const std::string kSparse = "sparse";
static const std::string kText = "text";
static const std::string kTTL = "ttl";
static const std::string kUnique = "unique";
static const std::string kWildcard = "wildcard";

const auto getAggregatedIndexUsageTracker =
    ServiceContext::declareDecoration<AggregatedIndexUsageTracker>();


template <class FuncPred>
void _updateStatsForEachFeature(const IndexFeatures& features,
                                AggregatedIndexUsageTracker::IndexStatsType& indexStats,
                                AggregatedIndexUsageTracker::FeatureStatsType& featureStats,
                                FuncPred update) {
    // Aggregate _id indexes separately so they do not get included with the other features.
    if (features.id) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kId)]);
        return;
    }

    update(indexStats[features.type]);

    if (features.collation) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kCollation)]);
    }

    if (features.compound) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kCompound)]);
    } else {
        update(featureStats[static_cast<size_t>(FeatureStatType::kSingle)]);
    }

    if (features.partial) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kPartial)]);
    }

    if (features.prepareUnique) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kPrepareUnique)]);
    }

    if (features.sparse) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kSparse)]);
    }

    if (features.ttl) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kTTL)]);
    }

    if (features.unique) {
        update(featureStats[static_cast<size_t>(FeatureStatType::kUnique)]);
    }
}

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
    features.prepareUnique = desc->prepareUnique();
    features.sparse = desc->isSparse();
    features.ttl = desc->infoObj().hasField(IndexDescriptor::kExpireAfterSecondsFieldName);
    features.type = indexType;
    features.unique = desc->unique();
    return features;
}

AggregatedIndexUsageTracker::AggregatedIndexUsageTracker() {}

AggregatedIndexUsageTracker* AggregatedIndexUsageTracker::get(ServiceContext* svcCtx) {
    return &getAggregatedIndexUsageTracker(svcCtx);
}

void AggregatedIndexUsageTracker::onAccess(const IndexFeatures& features) const {
    if (!features.internal) {
        _updateStatsForEachFeature(features, _indexTypeStats, _featureStats, [](auto& stats) {
            stats.accesses.fetchAndAdd(1);
        });
    }
}

void AggregatedIndexUsageTracker::onRegister(const IndexFeatures& features) const {
    if (!features.internal) {
        _updateStatsForEachFeature(features, _indexTypeStats, _featureStats, [](auto& stats) {
            stats.count.fetchAndAdd(1);
        });
        _count.fetchAndAdd(1);
    }
}

void AggregatedIndexUsageTracker::onUnregister(const IndexFeatures& features) const {
    if (!features.internal) {
        _updateStatsForEachFeature(features, _indexTypeStats, _featureStats, [](auto& stats) {
            stats.count.fetchAndAdd(-1);
        });
        _count.fetchAndAdd(-1);
    }
}

void AggregatedIndexUsageTracker::forEachFeature(OnFeatureFn&& onFeature) const {
    onFeature(k2d, _indexTypeStats[INDEX_2D]);
    onFeature(k2dSphere, _indexTypeStats[INDEX_2DSPHERE]);
    onFeature(k2dSphereBucket, _indexTypeStats[INDEX_2DSPHERE_BUCKET]);
    onFeature(kCollation, _featureStats[static_cast<size_t>(FeatureStatType::kCollation)]);
    onFeature(kCompound, _featureStats[static_cast<size_t>(FeatureStatType::kCompound)]);
    onFeature(kHashed, _indexTypeStats[INDEX_HASHED]);
    onFeature(kId, _featureStats[static_cast<size_t>(FeatureStatType::kId)]);
    onFeature(kNormal, _indexTypeStats[INDEX_BTREE]);
    onFeature(kPartial, _featureStats[static_cast<size_t>(FeatureStatType::kPartial)]);
    onFeature(kPrepareUnique, _featureStats[static_cast<size_t>(FeatureStatType::kPrepareUnique)]);
    onFeature(kSingle, _featureStats[static_cast<size_t>(FeatureStatType::kSingle)]);
    onFeature(kSparse, _featureStats[static_cast<size_t>(FeatureStatType::kSparse)]);
    onFeature(kText, _indexTypeStats[INDEX_TEXT]);
    onFeature(kTTL, _featureStats[static_cast<size_t>(FeatureStatType::kTTL)]);
    onFeature(kUnique, _featureStats[static_cast<size_t>(FeatureStatType::kUnique)]);
    onFeature(kWildcard, _indexTypeStats[INDEX_WILDCARD]);
}

long long AggregatedIndexUsageTracker::getCount() const {
    return _count.loadRelaxed();
}

void AggregatedIndexUsageTracker::resetToZero() {
    forEachFeature([](auto feature, const auto& constStats) {
        auto& stats = const_cast<IndexFeatureStats&>(constStats);
        stats.accesses.store(0);
        stats.count.store(0);
    });
    _count.store(0);
}

class IndexStatsSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

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
                featureBuilder.append("count", stats.count.loadRelaxed());
                featureBuilder.append("accesses", stats.accesses.loadRelaxed());
                featureBuilder.done();
            });
        featuresBuilder.done();
        return builder.obj();
    }
};

auto indexStatsSSS = *ServerStatusSectionBuilder<IndexStatsSSS>("indexStats").forShard();
}  // namespace mongo

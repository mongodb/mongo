// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ftdc/collection_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {
const auto ftdcCollectionMetrics = ServiceContext::declareDecoration<FTDCCollectionMetrics>();

class FTDCCollectionMetricsSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        return FTDCCollectionMetrics::get(opCtx).report();
    }
};

const auto& collectionMetricsSection =
    *ServerStatusSectionBuilder<FTDCCollectionMetricsSection>("ftdcCollectionMetrics")
         .forShard()
         .forRouter();
}  // namespace

FTDCCollectionMetrics& FTDCCollectionMetrics::get(ServiceContext* svcCtx) {
    return ftdcCollectionMetrics(svcCtx);
}

FTDCCollectionMetrics& FTDCCollectionMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void FTDCCollectionMetrics::onCompletingCollection(Microseconds duration) {
    const auto micros = durationCount<Microseconds>(duration);
    _collectionsCount.fetchAndAdd(1);
    _collectionsDurationMicros.fetchAndAdd(micros);
    if (micros >= _collectionPeriodMicros.load()) {
        _collectionsStalled.fetchAndAdd(1);
    }
}

BSONObj FTDCCollectionMetrics::report() const {
    BSONObjBuilder bob;
    bob.append(kCollectionsCountTag, static_cast<long long>(_collectionsCount.loadRelaxed()));
    bob.append(kCollectionsDurationTag,
               static_cast<long long>(_collectionsDurationMicros.loadRelaxed()));
    bob.append(kCollectionsStalledTag, static_cast<long long>(_collectionsStalled.loadRelaxed()));
    return bob.obj();
}

}  // namespace mongo

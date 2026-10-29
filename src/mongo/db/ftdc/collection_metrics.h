// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Collects metrics for FTDC collections. Users will call `onCompletingCollection` upon collecting
 * every sample and provide the callback with the time it took to collect the sample. Currently, the
 * only user is `FTDCCollectorCollection`, which reports collection metrics once it runs all
 * registered collectors.
 *
 * Observers can collect aggregate metrics using `report`, which returns an object similar to the
 * following: { "collections": 123, "durationMicros": 12345, "delayed": 0 } -- the only existing
 * observer is a serverStatus section, which reports these metrics under "ftdcCollectionMetrics".
 *
 * The number of stalled/delayed collections is updated with every collection that takes longer than
 * `_collectionPeriodMicros`, settable via `onUpdatingCollectionPeriod`.
 *
 * This class, except for its test-only APIs, is multi-thread safe, however, does not guarantee
 * happens before/after properties for reads and writes -- metrics may be partially updated when an
 * observer starts collecting them.
 */
class FTDCCollectionMetrics {
public:
    static constexpr auto kCollectionsCountTag = "collections"sv;
    static constexpr auto kCollectionsDurationTag = "durationMicros"sv;
    static constexpr auto kCollectionsStalledTag = "delayed"sv;

    FTDCCollectionMetrics() {
        _reset();
    }

    static FTDCCollectionMetrics& get(ServiceContext* svcCtx);
    static FTDCCollectionMetrics& get(OperationContext* opCtx);

    void onUpdatingCollectionPeriod(Microseconds period) {
        _collectionPeriodMicros.store(durationCount<Microseconds>(period));
    }

    void onCompletingCollection(Microseconds duration);

    BSONObj report() const;

    void reset_forTest() {
        _reset();
    }

private:
    void _reset() {
        _collectionPeriodMicros.store(0);
        _collectionsCount.store(0);
        _collectionsDurationMicros.store(0);
        _collectionsStalled.store(0);
    }

    Atomic<long long> _collectionPeriodMicros;
    Atomic<uint64_t> _collectionsCount;
    Atomic<uint64_t> _collectionsDurationMicros;
    Atomic<uint64_t> _collectionsStalled;
};

}  // namespace mongo

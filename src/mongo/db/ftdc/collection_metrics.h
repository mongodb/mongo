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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"

namespace mongo {

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
    static constexpr auto kCollectionsCountTag = "collections"_sd;
    static constexpr auto kCollectionsDurationTag = "durationMicros"_sd;
    static constexpr auto kCollectionsStalledTag = "delayed"_sd;

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

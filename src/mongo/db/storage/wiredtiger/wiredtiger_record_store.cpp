/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <wiredtiger.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/health_log_gen.h"
#include "mongo/db/catalog/health_log_interface.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/capped_snapshots.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/oplog_truncate_marker_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_truncate_markers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

using namespace fmt::literals;

namespace {

struct RecordIdAndWall {
    RecordId id;
    Date_t wall;

    RecordIdAndWall(RecordId lastRecord, Date_t wallTime)
        : id(std::move(lastRecord)), wall(wallTime) {}
};

WiredTigerRecordStore::CursorKey makeCursorKey(const RecordId& rid, KeyFormat format) {
    if (format == KeyFormat::Long) {
        return rid.getLong();
    } else {
        auto str = rid.getStr();
        return WiredTigerItem(str.rawData(), str.size());
    }
}

static const int kMinimumRecordStoreVersion = 1;
static const int kCurrentRecordStoreVersion = 1;  // New record stores use this by default.
static const int kMaximumRecordStoreVersion = 1;
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion >= kMinimumRecordStoreVersion);
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion <= kMaximumRecordStoreVersion);

const double kNumMSInHour = 1000 * 60 * 60;

static CompiledConfiguration lowerInclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=lower,inclusive=true");
static CompiledConfiguration lowerExclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=lower,inclusive=false");
static CompiledConfiguration upperInclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=upper,inclusive=true");
static CompiledConfiguration upperExclusiveBoundConfig("WT_CURSOR.bound",
                                                       "bound=upper,inclusive=false");
static CompiledConfiguration clearBoundConfig("WT_CURSOR.bound", "action=clear");

void checkOplogFormatVersion(OperationContext* opCtx, const std::string& uri) {
    StatusWith<BSONObj> appMetadata =
        WiredTigerUtil::getApplicationMetadata(*WiredTigerRecoveryUnit::get(opCtx), uri);
    fassert(39999, appMetadata);

    fassertNoTrace(39998, appMetadata.getValue().getIntField("oplogKeyExtractionVersion") == 1);
}

void appendNumericStats(WT_SESSION* s, const std::string& uri, BSONObjBuilder& bob) {
    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + uri, "statistics=(fast)", &bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }
}

std::size_t computeRecordIdSize(const RecordId& id) {
    // We previously weren't accounting for WiredTiger key size when it was an int64_t, thus we
    // return 0 in those cases. With the clustering capabilities we now support potentially large
    // keys as they are byte arrays, thus having to take it into account for the read/write metrics.
    return id.isStr() ? id.getStr().size() : 0;
}

boost::optional<NamespaceString> namespaceForUUID(OperationContext* opCtx,
                                                  const boost::optional<UUID>& uuid) {
    if (!uuid)
        return boost::none;

    // TODO SERVER-73111: Remove the dependency on CollectionCatalog
    return CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, *uuid);
}

RecordId getKey(WT_CURSOR* cursor, KeyFormat keyFormat) {
    if (keyFormat == KeyFormat::String) {
        WT_ITEM item;
        invariantWTOK(cursor->get_key(cursor, &item), cursor->session);
        return RecordId(static_cast<const char*>(item.data), item.size);
    } else {
        std::int64_t recordId;
        invariantWTOK(cursor->get_key(cursor, &recordId), cursor->session);
        return RecordId(recordId);
    }
}

RecordData getRecordData(WT_CURSOR* c) {
    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value), c->session);
    return {static_cast<const char*>(value.data), static_cast<int>(value.size)};
}


void setKey(WT_CURSOR* cursor, const WiredTigerRecordStore::CursorKey* key) {
    if (auto itemPtr = get_if<WiredTigerItem>(key)) {
        cursor->set_key(cursor, itemPtr->Get());
    } else if (auto longPtr = get_if<int64_t>(key)) {
        cursor->set_key(cursor, *longPtr);
    }
}

auto& boundRetries = *MetricBuilder<Counter64>("wiredTiger.recordStoreCursorBoundRetries");
}  // namespace

MONGO_FAIL_POINT_DEFINE(WTCompactRecordStoreEBUSY);
MONGO_FAIL_POINT_DEFINE(WTRecordStoreUassertOutOfOrder);
MONGO_FAIL_POINT_DEFINE(WTWriteConflictException);
MONGO_FAIL_POINT_DEFINE(WTWriteConflictExceptionForReads);

std::shared_ptr<WiredTigerRecordStore::OplogTruncateMarkers>
WiredTigerRecordStore::OplogTruncateMarkers::createEmptyOplogTruncateMarkers(
    WiredTigerRecordStore* rs) {
    return std::make_shared<WiredTigerRecordStore::OplogTruncateMarkers>(
        std::deque<CollectionTruncateMarkers::Marker>{},
        0,
        0,
        0,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::InProgress,
        rs);
}

std::shared_ptr<WiredTigerRecordStore::OplogTruncateMarkers>
WiredTigerRecordStore::OplogTruncateMarkers::sampleAndUpdate(OperationContext* opCtx,
                                                             WiredTigerRecordStore* rs,
                                                             const NamespaceString& ns) {
    // Sample
    long long maxSize = rs->_oplogMaxSize.load();
    invariant(rs->_isCapped && rs->_isOplog);
    invariant(maxSize > 0);
    invariant(rs->keyFormat() == KeyFormat::Long);

    // The minimum oplog truncate marker size should be BSONObjMaxInternalSize.
    const unsigned int oplogTruncateMarkerSize =
        std::max(gOplogTruncateMarkerSizeMB * 1024 * 1024, BSONObjMaxInternalSize);

    // IDL does not support unsigned long long types.
    const unsigned long long kMinTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMinOplogTruncateMarkers);
    const unsigned long long kMaxTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMaxOplogTruncateMarkersDuringStartup);

    unsigned long long numTruncateMarkers = maxSize / oplogTruncateMarkerSize;
    size_t numTruncateMarkersToKeep = std::min(
        kMaxTruncateMarkersToKeep, std::max(kMinTruncateMarkersToKeep, numTruncateMarkers));
    auto minBytesPerTruncateMarker = maxSize / numTruncateMarkersToKeep;
    uassert(7206300,
            fmt::format("Cannot create oplog of size less than {} bytes", numTruncateMarkersToKeep),
            minBytesPerTruncateMarker > 0);

    // We need to read the whole oplog, override the recoveryUnit's oplogVisibleTimestamp.
    ScopedOplogVisibleTimestamp scopedOplogVisibleTimestamp(
        shard_role_details::getRecoveryUnit(opCtx), boost::none);
    UnyieldableCollectionIterator iterator(opCtx, rs);
    auto initialSetOfMarkers = CollectionTruncateMarkers::createFromCollectionIterator(
        opCtx,
        iterator,
        ns,
        minBytesPerTruncateMarker,
        [](const Record& record) {
            BSONObj obj = record.data.toBson();
            auto wallTime = obj.hasField(repl::DurableOplogEntry::kWallClockTimeFieldName)
                ? obj[repl::DurableOplogEntry::kWallClockTimeFieldName].Date()
                : obj[repl::DurableOplogEntry::kTimestampFieldName].timestampTime();
            return RecordIdAndWallTime(record.id, wallTime);
        },
        numTruncateMarkersToKeep);
    LOGV2(22382,
          "WiredTiger record store oplog processing finished",
          "duration"_attr = duration_cast<Milliseconds>(initialSetOfMarkers.timeTaken));
    LOGV2(10621110,
          "Initial set of markers created.",
          "Oplog size (in bytes)"_attr = rs->dataSize(opCtx));

    // This value will eventually replace the empty OplogTruncateMarker object with this newly
    // populated object now that initial sampling has finished.
    auto otm = std::make_shared<OplogTruncateMarkers>(std::move(initialSetOfMarkers.markers),
                                                      initialSetOfMarkers.leftoverRecordsCount,
                                                      initialSetOfMarkers.leftoverRecordsBytes,
                                                      minBytesPerTruncateMarker,
                                                      initialSetOfMarkers.timeTaken,
                                                      initialSetOfMarkers.methodUsed,
                                                      rs);
    otm->initialSamplingFinished();
    return otm;
}

std::shared_ptr<WiredTigerRecordStore::OplogTruncateMarkers>
WiredTigerRecordStore::OplogTruncateMarkers::createOplogTruncateMarkers(OperationContext* opCtx,
                                                                        WiredTigerRecordStore* rs,
                                                                        const NamespaceString& ns) {
    LOGV2(10621000,
          "Creating oplog markers",
          "sampling asynchronously"_attr = gOplogSamplingAsyncEnabled);
    if (!gOplogSamplingAsyncEnabled) {
        return sampleAndUpdate(opCtx, rs, ns);
    }
    return createEmptyOplogTruncateMarkers(rs);
}

WiredTigerRecordStore::OplogTruncateMarkers::OplogTruncateMarkers(
    std::deque<CollectionTruncateMarkers::Marker> markers,
    int64_t partialMarkerRecords,
    int64_t partialMarkerBytes,
    int64_t minBytesPerMarker,
    Microseconds totalTimeSpentBuilding,
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod,
    WiredTigerRecordStore* rs)
    : CollectionTruncateMarkers(
          std::move(markers), partialMarkerRecords, partialMarkerBytes, minBytesPerMarker),
      _rs(rs),
      _totalTimeProcessing(totalTimeSpentBuilding),
      _creationMethod(creationMethod) {}

bool WiredTigerRecordStore::OplogTruncateMarkers::isDead() {
    stdx::lock_guard<Latch> lk(_reclaimMutex);
    return _isDead;
}

void WiredTigerRecordStore::OplogTruncateMarkers::kill() {
    stdx::lock_guard<Latch> lk(_reclaimMutex);
    _isDead = true;
    _reclaimCv.notify_one();
}

void WiredTigerRecordStore::OplogTruncateMarkers::clearMarkersOnCommit(OperationContext* opCtx) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [this](OperationContext*, boost::optional<Timestamp>) {
            modifyMarkersWith([&](std::deque<CollectionTruncateMarkers::Marker>& markers) {
                markers.clear();
                modifyPartialMarker([&](CollectionTruncateMarkers::PartialMarkerMetrics metrics) {
                    metrics.currentRecords->store(0);
                    metrics.currentBytes->store(0);
                });
            });
        });
}

void WiredTigerRecordStore::OplogTruncateMarkers::updateMarkersAfterCappedTruncateAfter(
    int64_t recordsRemoved, int64_t bytesRemoved, const RecordId& firstRemovedId) {
    modifyMarkersWith([&](std::deque<CollectionTruncateMarkers::Marker>& markers) {
        int64_t numMarkersToRemove = 0;
        int64_t recordsInMarkersToRemove = 0;
        int64_t bytesInMarkersToRemove = 0;

        // Compute the number and associated sizes of the records from markers that are either fully
        // or partially truncated.
        for (auto it = markers.rbegin(); it != markers.rend(); ++it) {
            if (it->lastRecord < firstRemovedId) {
                break;
            }
            numMarkersToRemove++;
            recordsInMarkersToRemove += it->records;
            bytesInMarkersToRemove += it->bytes;
        }

        // Remove the markers corresponding to the records that were deleted.
        int64_t offset = markers.size() - numMarkersToRemove;
        markers.erase(markers.begin() + offset, markers.end());

        // Account for any remaining records from a partially truncated marker in the marker
        // currently being filled.
        modifyPartialMarker([&](CollectionTruncateMarkers::PartialMarkerMetrics metrics) {
            metrics.currentRecords->fetchAndAdd(recordsInMarkersToRemove - recordsRemoved);
            metrics.currentBytes->fetchAndAdd(bytesInMarkersToRemove - bytesRemoved);
        });
    });
}

void WiredTigerRecordStore::OplogTruncateMarkers::getOplogTruncateMarkersStats(
    BSONObjBuilder& builder) const {
    builder.append("totalTimeProcessingMicros", _totalTimeProcessing.count());
    if (_creationMethod == CollectionTruncateMarkers::MarkersCreationMethod::Sampling) {
        builder.append("processingMethod", "sampling");
    } else if (_creationMethod == CollectionTruncateMarkers::MarkersCreationMethod::InProgress) {
        builder.append("processingMethod", "in progress");
    } else if (_creationMethod ==
                   CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection ||
               _creationMethod == CollectionTruncateMarkers::MarkersCreationMethod::Scanning) {
        builder.append("processingMethod", "scanning");
    }

    if (auto oplogMinRetentionHours = storageGlobalParams.oplogMinRetentionHours.load()) {
        builder.append("oplogMinRetentionHours", oplogMinRetentionHours);
    }
}

bool WiredTigerRecordStore::OplogTruncateMarkers::awaitHasExcessMarkersOrDead(
    OperationContext* opCtx) {
    // Wait until kill() is called or there are too many collection markers.
    stdx::unique_lock<Latch> lock(_reclaimMutex);
    MONGO_IDLE_THREAD_BLOCK;
    LOGV2_DEBUG(10621102, 1, "OplogCapMaintainerThread is idle");
    auto isWaitConditionSatisfied = opCtx->waitForConditionOrInterruptFor(
        _reclaimCv, lock, Seconds(gOplogTruncationCheckPeriodSeconds), [this, opCtx] {
            if (_isDead) {
                LOGV2_DEBUG(10621103, 1, "OplogCapMaintainerThread is active");
                return true;
            }

            if (auto marker = peekOldestMarkerIfNeeded(opCtx)) {
                invariant(marker->lastRecord.isValid());

                LOGV2_DEBUG(7393215,
                            2,
                            "Collection has excess markers",
                            "lastRecord"_attr = marker->lastRecord,
                            "wallTime"_attr = marker->wallTime);
                LOGV2_DEBUG(10621104, 1, "OplogCapMaintainerThread is active");
                return true;
            }

            LOGV2_DEBUG(10621105, 1, "OplogCapMaintainerThread is active");
            return false;
        });

    LOGV2_DEBUG(10621106, 1, "OplogCapMaintainerThread is active");
    // Return true only when we have detected excess markers, not because the record store
    // is being destroyed (_isDead) or we timed out waiting on the condition variable.
    return !(_isDead || !isWaitConditionSatisfied);
}

bool WiredTigerRecordStore::OplogTruncateMarkers::_hasExcessMarkers(OperationContext* opCtx) const {
    int64_t totalBytes = 0;
    for (const auto& marker : getMarkers()) {
        totalBytes += marker.bytes;
    }

    // check that oplog truncate markers is at capacity
    if (totalBytes <= _rs->_oplogMaxSize.load()) {
        return false;
    }

    const auto& truncateMarker = getMarkers().front();

    // The pinned oplog is inside the earliest marker, so we cannot remove the marker range.
    if (static_cast<std::uint64_t>(truncateMarker.lastRecord.getLong()) >=
        _rs->getPinnedOplog().asULL()) {
        return false;
    }

    double minRetentionHours = storageGlobalParams.oplogMinRetentionHours.load();

    // If we are not checking for time, then yes, there is a truncate marker to be reaped
    // because oplog is at capacity.
    if (minRetentionHours == 0.0) {
        return true;
    }

    auto nowWall = Date_t::now();
    auto lastTruncateMarkerWallTime = truncateMarker.wallTime;

    auto currRetentionMS = durationCount<Milliseconds>(nowWall - lastTruncateMarkerWallTime);
    double currRetentionHours = currRetentionMS / kNumMSInHour;
    return currRetentionHours >= minRetentionHours;
}

void WiredTigerRecordStore::OplogTruncateMarkers::adjust(OperationContext* opCtx, int64_t maxSize) {
    const unsigned int oplogTruncateMarkerSize =
        std::max(gOplogTruncateMarkerSizeMB * 1024 * 1024, BSONObjMaxInternalSize);

    // IDL does not support unsigned long long types.
    const unsigned long long kMinTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMinOplogTruncateMarkers);
    const unsigned long long kMaxTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMaxOplogTruncateMarkersAfterStartup);

    unsigned long long numTruncateMarkers = maxSize / oplogTruncateMarkerSize;
    size_t numTruncateMarkersToKeep = std::min(
        kMaxTruncateMarkersToKeep, std::max(kMinTruncateMarkersToKeep, numTruncateMarkers));
    setMinBytesPerMarker(maxSize / numTruncateMarkersToKeep);
    // Notify the reclaimer thread as there might be an opportunity to recover space.
    _reclaimCv.notify_all();
}

StatusWith<std::string> WiredTigerRecordStore::parseOptionsField(const BSONObj options) {
    StringBuilder ss;
    BSONForEach(elem, options) {
        if (elem.fieldNameStringData() == WiredTigerUtil::kConfigStringField) {
            Status status = WiredTigerUtil::checkTableCreationOptions(elem);
            if (!status.isOK()) {
                return status;
            }
            ss << elem.valueStringData() << ',';
        } else {
            // Return error on first unrecognized field.
            return StatusWith<std::string>(ErrorCodes::InvalidOptions,
                                           str::stream() << '\'' << elem.fieldNameStringData()
                                                         << '\'' << " is not a supported option.");
        }
    }
    return StatusWith<std::string>(ss.str());
}

class WiredTigerRecordStore::RandomCursor final : public RecordCursor {
public:
    RandomCursor(OperationContext* opCtx, const WiredTigerRecordStore& rs)
        : _cursor(nullptr),
          _keyFormat(rs._keyFormat),
          _uri(rs._uri),
          _opCtx(opCtx),
          _tableId(rs._tableId) {
        restore();
    }

    ~RandomCursor() override = default;

    boost::optional<Record> next() final {
        int advanceRet = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return _cursor->get()->next(_cursor->get()); });
        if (advanceRet == WT_NOTFOUND)
            return {};
        invariantWTOK(advanceRet, _cursor->getSession()->getSession());

        RecordId id;
        if (_keyFormat == KeyFormat::String) {
            WT_ITEM item;
            invariantWTOK(_cursor->get()->get_key(_cursor->get(), &item),
                          _cursor->getSession()->getSession());
            id = RecordId(static_cast<const char*>(item.data), item.size);
        } else {
            int64_t key;
            invariantWTOK(_cursor->get()->get_key(_cursor->get(), &key),
                          _cursor->getSession()->getSession());
            id = RecordId(key);
        }

        WT_ITEM value;
        invariantWTOK(_cursor->get()->get_value(_cursor->get(), &value),
                      _cursor->getSession()->getSession());

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);

        auto keyLength = computeRecordIdSize(id);
        metricsCollector.incrementOneDocRead(_uri, value.size + keyLength);


        return {
            {std::move(id), {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
    }

    void save() final {
        if (_cursor) {
            invariantWTOK(WT_READ_CHECK(_cursor->get()->reset(_cursor->get())),
                          _cursor->getSession()->getSession());
        }
    }

    bool restore(bool tolerateCappedRepositioning = true) final {
        WiredTigerRecoveryUnit* wtRu = WiredTigerRecoveryUnit::get(_opCtx);

        if (!_cursor) {
            _cursor = std::make_unique<WiredTigerCursor>(
                *wtRu, _uri, _tableId, /*allowOverwrite=*/false, /*random=*/true);
        }
        return true;
    }

    void detachFromOperationContext() final {
        invariant(_opCtx);
        _opCtx = nullptr;
        if (!_saveStorageCursorOnDetachFromOperationContext) {
            _cursor.reset();
        }
    }

    void reattachToOperationContext(OperationContext* opCtx) final {
        invariant(!_opCtx);
        _opCtx = opCtx;
    }

    void setSaveStorageCursorOnDetachFromOperationContext(bool saveCursor) override {
        _saveStorageCursorOnDetachFromOperationContext = saveCursor;
    }

private:
    std::unique_ptr<WiredTigerCursor> _cursor;
    KeyFormat _keyFormat;
    const std::string _uri;
    OperationContext* _opCtx;
    const uint64_t _tableId;
    bool _saveStorageCursorOnDetachFromOperationContext = false;
};


// static
StatusWith<std::string> WiredTigerRecordStore::generateCreateString(
    const std::string& engineName,
    const NamespaceString& nss,
    StringData ident,
    const CollectionOptions& options,
    StringData extraStrings,
    KeyFormat keyFormat,
    bool loggingEnabled) {

    // Separate out a prefix and suffix in the default string. User configuration will
    // override values in the prefix, but not values in the suffix.
    str::stream ss;
    ss << "type=file,";
    // Setting this larger than 10m can hurt latencies and throughput degradation if this
    // is the oplog.  See SERVER-16247
    ss << "memory_page_max=10m,";
    // Choose a higher split percent, since most usage is append only. Allow some space
    // for workloads where updates increase the size of documents.
    ss << "split_pct=90,";
    ss << "leaf_value_max=64MB,";

    ss << "checksum=on,";
    if (wiredTigerGlobalOptions.useCollectionPrefixCompression) {
        ss << "prefix_compression,";
    }

    ss << "block_compressor=";
    if (options.timeseries) {
        // Time-series collections use zstd compression by default.
        ss << WiredTigerGlobalOptions::kDefaultTimeseriesCollectionCompressor;
    } else {
        // All other collections use the globally configured default.
        ss << wiredTigerGlobalOptions.collectionBlockCompressor;
    }
    ss << ",";

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())
              ->getTableCreateConfig(NamespaceStringUtil::serializeForCatalog(nss));

    ss << extraStrings << ",";

    StatusWith<std::string> customOptions =
        parseOptionsField(options.storageEngine.getObjectField(engineName));
    if (!customOptions.isOK())
        return customOptions;

    ss << customOptions.getValue();

    if (nss.isOplog()) {
        // force file for oplog
        ss << "type=file,";
        // Tune down to 10m.  See SERVER-16247
        ss << "memory_page_max=10m,";
    }

    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.
    if (options.clusteredIndex) {
        // A clustered collection requires both CollectionOptions.clusteredIndex and
        // KeyFormat::String. For a clustered record store that is not associated with a clustered
        // collection KeyFormat::String is sufficient.
        uassert(6144101,
                "RecordStore with CollectionOptions.clusteredIndex requires KeyFormat::String",
                keyFormat == KeyFormat::String);
    }
    if (keyFormat == KeyFormat::String) {
        // If the RecordId format is a String, assume a byte array key format.
        ss << "key_format=u";
    } else {
        // All other collections use an int64_t as their table keys.
        ss << "key_format=q";
    }
    ss << ",value_format=u";

    // Record store metadata
    ss << ",app_metadata=(formatVersion=" << kCurrentRecordStoreVersion;
    if (nss.isOplog()) {
        ss << ",oplogKeyExtractionVersion=1";
    }
    ss << ")";

    if (loggingEnabled) {
        ss << ",log=(enabled=true)";
    } else {
        ss << ",log=(enabled=false)";
    }

    return StatusWith<std::string>(ss);
}

WiredTigerRecordStore::WiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                             OperationContext* ctx,
                                             Params params)
    : RecordStore(params.uuid, params.ident, params.isCapped),
      _uri(WiredTigerKVEngine::kTableUriPrefix + params.ident),
      _tableId(WiredTigerSession::genTableId()),
      _engineName(params.engineName),
      _isCapped(params.isCapped),
      _keyFormat(params.keyFormat),
      _overwrite(params.overwrite),
      _isEphemeral(params.isEphemeral),
      _isLogged(params.isLogged),
      _isOplog(params.nss.isOplog()),
      _isChangeCollection(params.nss.isChangeCollection()),
      _forceUpdateWithFullDocument(params.forceUpdateWithFullDocument),
      _oplogMaxSize(params.oplogMaxSize),
      _sizeStorer(params.sizeStorer),
      _tracksSizeAdjustments(params.tracksSizeAdjustments),
      _kvEngine(kvEngine) {
    invariant(getIdent().size() > 0);

    if (kDebugBuild && _keyFormat == KeyFormat::String) {
        // This is a clustered record store. Its WiredTiger table requires key_format='u' for
        // correct operation.
        const std::string wtTableConfig = uassertStatusOK(
            WiredTigerUtil::getMetadataCreate(*WiredTigerRecoveryUnit::get(ctx), _uri));
        const bool wtTableConfigMatchesStringKeyFormat =
            wtTableConfig.find("key_format=u") != std::string::npos;
        invariant(wtTableConfigMatchesStringKeyFormat);
    }

    if (_oplogMaxSize.load()) {
        invariant(_isOplog, str::stream() << "Namespace " << params.nss.toStringForErrorMsg());
    }

    Status versionStatus =
        WiredTigerUtil::checkApplicationMetadataFormatVersion(*WiredTigerRecoveryUnit::get(ctx),
                                                              _uri,
                                                              kMinimumRecordStoreVersion,
                                                              kMaximumRecordStoreVersion)
            .getStatus();

    if (!versionStatus.isOK()) {
        LOGV2_ERROR(7887900,
                    "Metadata format version check failed.",
                    "uri"_attr = _uri,
                    "namespace"_attr = params.nss.toStringForErrorMsg(),
                    "version"_attr = versionStatus.reason());
        if (versionStatus.code() == ErrorCodes::FailedToParse) {
            uasserted(28548, versionStatus.reason());
        } else {
            fassertFailedNoTrace(34433);
        }
    }

    uassertStatusOK(
        WiredTigerUtil::setTableLogging(*WiredTigerRecoveryUnit::get(ctx), _uri, _isLogged));

    if (_isOplog) {
        invariant(_keyFormat == KeyFormat::Long);
        checkOplogFormatVersion(ctx, _uri);
        // The oplog always needs to be marked for size adjustment since it is journaled and also
        // may change during replication recovery (if truncated).
        sizeRecoveryState(getGlobalServiceContext())
            .markCollectionAsAlwaysNeedsSizeAdjustment(getIdent());
    }

    // If no SizeStorer is in use, start counting at zero. In practice, this will only ever be the
    // case for temporary RecordStores (those not associated with any collection) and in unit
    // tests. Persistent size information is not required in either case. If a RecordStore needs
    // persistent size information, we require it to use a SizeStorer.
    _sizeInfo = _sizeStorer ? _sizeStorer->load(_uri)
                            : std::make_shared<WiredTigerSizeStorer::SizeInfo>(0, 0);
}

WiredTigerRecordStore::~WiredTigerRecordStore() {
    if (!isTemp()) {
        LOGV2_DEBUG(
            22395, 1, "~WiredTigerRecordStore", "ident"_attr = getIdent(), "uuid"_attr = uuid());
    } else {
        LOGV2_DEBUG(22396,
                    1,
                    "~WiredTigerRecordStore for temporary ident: {getIdent}",
                    "getIdent"_attr = getIdent());
    }

    {
        std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
        if (_oplogTruncateMarkers) {
            _oplogTruncateMarkers->kill();
        }
    }

    if (_isOplog) {
        // Delete oplog visibility manager on KV engine.
        _kvEngine->haltOplogManager(this, /*shuttingDown=*/false);
    }
}

NamespaceString WiredTigerRecordStore::ns(OperationContext* opCtx) const {
    auto nss = namespaceForUUID(opCtx, _uuid);

    return nss ? *nss : NamespaceString::kEmpty;
}

void WiredTigerRecordStore::checkSize(OperationContext* opCtx) {
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, /*forward=*/true);
    if (!cursor->next()) {
        // We found no records in this collection; however, there may actually be documents present
        // if writes to this collection were not included in the stable checkpoint the last time
        // this node shut down. We set the data size and the record count to zero, but will adjust
        // these if writes are played during startup recovery.
        // Alternatively, this may be a collection we are creating during replication recovery.
        // In that case the collection will be given a new ident and a new SizeStorer entry. The
        // collection size from before we recovered to stable timestamp is not associated with this
        // record store and so we must keep track of the count throughout recovery.
        //
        // We mark a RecordStore as needing size adjustment iff its size is accurate at the current
        // time but not as of the top of the oplog.
        LOGV2_FOR_RECOVERY(23983,
                           2,
                           "Record store was empty; setting count metadata to zero but marking "
                           "record store as needing size adjustment during recovery. ns: "
                           "{isTemp_temp_ns}, ident: {ident}",
                           "isTemp_temp_ns"_attr =
                               (isTemp() ? "(temp)" : toStringForLogging(ns(opCtx))),
                           "ident"_attr = getIdent());
        sizeRecoveryState(getGlobalServiceContext())
            .markCollectionAsAlwaysNeedsSizeAdjustment(getIdent());
        _sizeInfo->dataSize.store(0);
        _sizeInfo->numRecords.store(0);
    }

    if (_sizeStorer)
        _sizeStorer->store(_uri, _sizeInfo);
}

void WiredTigerRecordStore::postConstructorInit(OperationContext* opCtx,
                                                const NamespaceString& ns) {
    // If the server was started in read-only mode, if we are restoring the node, or if async
    // sampling is enabled, skip calculating the oplog truncate markers here.
    if (_isOplog && opCtx->getServiceContext()->userWritesAllowed() &&
        !storageGlobalParams.repair && !repl::ReplSettings::shouldSkipOplogSampling() &&
        !gOplogSamplingAsyncEnabled) {
        std::lock_guard lk(_oplogTruncateMarkersMutex);
        _oplogTruncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, this, ns);
    }

    if (_isOplog) {
        invariant(_kvEngine);
        _kvEngine->startOplogManager(opCtx, this);
    }
}

void WiredTigerRecordStore::getOplogTruncateStats(BSONObjBuilder& builder) const {
    std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
    if (_oplogTruncateMarkers) {
        _oplogTruncateMarkers->getOplogTruncateMarkersStats(builder);
    }
    builder.append("totalTimeTruncatingMicros", _totalTimeTruncating.load());
    builder.append("truncateCount", _truncateCount.load());
}

const char* WiredTigerRecordStore::name() const {
    return _engineName.c_str();
}

KeyFormat WiredTigerRecordStore::keyFormat() const {
    return _keyFormat;
}

long long WiredTigerRecordStore::dataSize(OperationContext* opCtx) const {
    auto dataSize = _sizeInfo->dataSize.load();
    return dataSize > 0 ? dataSize : 0;
}

long long WiredTigerRecordStore::numRecords(OperationContext* opCtx) const {
    auto numRecords = _sizeInfo->numRecords.load();
    return numRecords > 0 ? numRecords : 0;
}

int64_t WiredTigerRecordStore::storageSize(OperationContext* opCtx,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel) const {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());

    if (_isEphemeral) {
        return dataSize(opCtx);
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    auto result = WiredTigerUtil::getStatisticsValue(session->getSession(),
                                                     "statistics:" + getURI(),
                                                     "statistics=(size)",
                                                     WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());

    int64_t size = result.getValue();

    if (size == 0 && _isCapped) {
        // Many things assume an empty capped collection still takes up space.
        return 1;
    }
    return size;
}

int64_t WiredTigerRecordStore::freeStorageSize(OperationContext* opCtx) const {
    invariant(shard_role_details::getLocker(opCtx)->isReadLocked());

    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    return WiredTigerUtil::getIdentReuseSize(session->getSession(), getURI());
}

void WiredTigerRecordStore::_updateLargestRecordId(OperationContext* opCtx, long long largestSeen) {
    invariant(_keyFormat == KeyFormat::Long);
    invariant(!_isOplog);

    // Make sure to inialize first; otherwise the compareAndSwap can succeed trivially.
    _initNextIdIfNeeded(opCtx);

    // Since the 'largestSeen' is the largest we've seen, we need to set the _nextIdNum to one
    // higher than that: to 'largestSeen + 1'. This is because if we assign recordIds,
    // we start at _nextIdNum. Therefore if it was set to 'largestSeen', it would clash with
    // the current largest recordId.
    largestSeen++;
    auto nextIdNum = _nextIdNum.load();
    while (largestSeen > nextIdNum && !_nextIdNum.compareAndSwap(&nextIdNum, largestSeen)) {
    }
}

// Retrieve the value from a positioned cursor.
RecordData WiredTigerRecordStore::_getData(const WiredTigerCursor& cursor) const {
    WT_ITEM value;
    invariantWTOK(cursor->get_value(cursor.get(), &value), cursor->session);
    return RecordData(static_cast<const char*>(value.data), value.size).getOwned();
}

void WiredTigerRecordStore::doDeleteRecord(OperationContext* opCtx, const RecordId& id) {
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // SERVER-48453: Initialize the next record id counter before deleting. This ensures we won't
    // reuse record ids, which can be problematic for the _mdb_catalog.
    if (_keyFormat == KeyFormat::Long) {
        _initNextIdIfNeeded(opCtx);
    }

    WiredTigerCursor cursor(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    cursor.assertInActiveTxn();
    WT_CURSOR* c = cursor.get();
    CursorKey key = makeCursorKey(id, _keyFormat);
    setKey(c, &key);
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });
    if (ret == WT_NOTFOUND) {
        HealthLogEntry entry;
        entry.setNss(namespaceForUUID(opCtx, _uuid));
        entry.setTimestamp(Date_t::now());
        entry.setSeverity(SeverityEnum::Error);
        entry.setScope(ScopeEnum::Collection);
        entry.setOperation("WT_Cursor::remove");
        entry.setMsg("Record to be deleted not found");

        BSONObjBuilder bob;
        bob.append("RecordId", id.toString());
        bob.append("ident", getIdent());
        bob.appendElements(getStackTrace().getBSONRepresentation());
        entry.setData(bob.obj());

        HealthLogInterface::get(opCtx)->log(entry);

        if (TestingProctor::instance().isEnabled()) {
            LOGV2_FATAL(9099700,
                        "Record to be deleted not found",
                        "nss"_attr = namespaceForUUID(opCtx, _uuid),
                        "RecordId"_attr = id);
        } else {
            // Return early without crash if in production.
            LOGV2_ERROR(9099701,
                        "Record to be deleted not found",
                        "nss"_attr = namespaceForUUID(opCtx, _uuid),
                        "RecordId"_attr = id);
            printStackTrace();
            return;
        }
    }
    invariantWTOK(ret, c->session);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret, c->session);

    int64_t old_length = old_value.size;

    ret = WT_OP_CHECK(wiredTigerCursorRemove(*WiredTigerRecoveryUnit::get(opCtx), c));
    invariantWTOK(ret, c->session);

    auto keyLength = computeRecordIdSize(id);
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneDocWritten(_uri, old_length + keyLength);

    _changeNumRecordsAndDataSize(opCtx, -1, -old_length);
}

Timestamp WiredTigerRecordStore::getPinnedOplog() const {
    return _kvEngine->getPinnedOplog();
}

std::shared_ptr<CollectionTruncateMarkers> WiredTigerRecordStore::getCollectionTruncateMarkers() {
    std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
    return _oplogTruncateMarkers->shared_from_this();
}

void WiredTigerRecordStore::reclaimOplog(OperationContext* opCtx) {
    auto mayTruncateUpTo = _kvEngine->getPinnedOplog();
    invariant(_keyFormat == KeyFormat::Long);

    Timer timer;
    for (auto getNextMarker = true; getNextMarker;) {
        std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
        auto truncateMarker = _oplogTruncateMarkers->peekOldestMarkerIfNeeded(opCtx);
        if (!truncateMarker) {
            break;
        }
        invariant(truncateMarker->lastRecord.isValid());

        LOGV2_DEBUG(7420100,
                    1,
                    "Truncating the oplog",
                    "firstRecord"_attr = _oplogTruncateMarkers->firstRecord,
                    "lastRecord"_attr = truncateMarker->lastRecord,
                    "numRecords"_attr = truncateMarker->records,
                    "numBytes"_attr = truncateMarker->bytes);

        WiredTigerRecoveryUnit* ru = WiredTigerRecoveryUnit::get(opCtx);
        WT_SESSION* session = ru->getSession()->getSession();

        writeConflictRetry(opCtx, "reclaimOplog", NamespaceString::kRsOplogNamespace, [&] {
            WriteUnitOfWork wuow(opCtx);

            WiredTigerCursor cwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
            WT_CURSOR* cursor = cwrap.get();

            // The first record in the oplog should be within the truncate range.
            int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return cursor->next(cursor); });
            invariantWTOK(ret, cursor->session);
            RecordId firstRecord = getKey(cursor, _keyFormat);
            if (firstRecord < _oplogTruncateMarkers->firstRecord ||
                firstRecord > truncateMarker->lastRecord) {
                LOGV2_WARNING(7420101,
                              "First oplog record is not in truncation range",
                              "firstRecord"_attr = firstRecord,
                              "truncateRangeFirstRecord"_attr = _oplogTruncateMarkers->firstRecord,
                              "truncateRangeLastRecord"_attr = truncateMarker->lastRecord);
            }

            // It is necessary that there exists a record after the truncate marker but before or
            // including the mayTruncateUpTo point.  Since the mayTruncateUpTo point may fall
            // between records, the truncate marker check is not sufficient.
            CursorKey truncateUpToKey = makeCursorKey(truncateMarker->lastRecord, _keyFormat);
            setKey(cursor, &truncateUpToKey);
            int cmp;
            ret = wiredTigerPrepareConflictRetry(opCtx,
                                                 [&] { return cursor->search_near(cursor, &cmp); });
            invariantWTOK(ret, cursor->session);

            // Check 'cmp' to determine if we landed on the requested record. While it is often the
            // case that truncate markers represent a perfect partitioning of the oplog, it's not
            // guaranteed.  The truncation method is lenient to overlapping truncate markers. See
            // SERVER-56590 for details.  If we landed land on a higher record (cmp > 0), we likely
            // truncated a duplicate truncate marker in a previous iteration. In this case we can
            // skip the check for oplog entries after the truncate marker we are truncating. If we
            // landed on a prior record, then we have records that are not in truncation range of
            // any truncate marker. This will have been logged as a warning, above.
            if (cmp <= 0) {
                ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return cursor->next(cursor); });
                if (ret == WT_NOTFOUND) {
                    LOGV2_DEBUG(5140900, 0, "Will not truncate entire oplog");
                    getNextMarker = false;
                    return;
                }
                invariantWTOK(ret, cursor->session);
            }
            RecordId nextRecord = getKey(cursor, _keyFormat);
            if (static_cast<std::uint64_t>(nextRecord.getLong()) > mayTruncateUpTo.asULL()) {
                LOGV2_DEBUG(
                    5140901,
                    0,
                    "Cannot truncate as there are no oplog entries after the truncate marker but "
                    "before the truncate-up-to point",
                    "nextRecord"_attr = Timestamp(nextRecord.getLong()),
                    "mayTruncateUpTo"_attr = mayTruncateUpTo);
                getNextMarker = false;
                return;
            }

            // After checking whether or not we should truncate, reposition the cursor back to the
            // current truncate marker's lastRecord.
            invariantWTOK(cursor->reset(cursor), cursor->session);
            setKey(cursor, &truncateUpToKey);
            invariantWTOK(session->truncate(session, nullptr, nullptr, cursor, nullptr), session);
            _changeNumRecordsAndDataSize(opCtx, -truncateMarker->records, -truncateMarker->bytes);

            wuow.commit();

            // Remove the truncate marker after a successful truncation.
            _oplogTruncateMarkers->popOldestMarker();

            // Stash the truncate point for next time to cleanly skip over tombstones, etc.
            _oplogTruncateMarkers->firstRecord = truncateMarker->lastRecord;
            Timestamp firstRecordTimestamp{
                static_cast<uint64_t>(truncateMarker->lastRecord.getLong())};
            _oplogFirstRecordTimestamp.store(firstRecordTimestamp);
        });
    }

    auto elapsedMicros = timer.micros();
    auto elapsedMillis = elapsedMicros / 1000;
    _totalTimeTruncating.fetchAndAdd(elapsedMicros);
    _truncateCount.fetchAndAdd(1);
    LOGV2(22402,
          "WiredTiger record store oplog truncation finished",
          "pinnedOplogTimestamp"_attr = mayTruncateUpTo,
          "numRecords"_attr = _sizeInfo->numRecords.load(),
          "dataSize"_attr = _sizeInfo->dataSize.load(),
          "duration"_attr = Milliseconds(elapsedMillis));
}

Status WiredTigerRecordStore::doInsertRecords(OperationContext* opCtx,
                                              std::vector<Record>* records,
                                              const std::vector<Timestamp>& timestamps) {
    return _insertRecords(opCtx, records->data(), timestamps.data(), records->size());
}

Status WiredTigerRecordStore::_insertRecords(OperationContext* opCtx,
                                             Record* records,
                                             const Timestamp* timestamps,
                                             size_t nRecords) {
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    WiredTigerCursor curwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, _overwrite);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);

    invariant(nRecords != 0);

    if (_keyFormat == KeyFormat::Long) {
        bool areRecordIdsProvided = !records->id.isNull() && !_isOplog;
        RecordId highestRecordIdProvided;

        long long nextId =
            (_isOplog || areRecordIdsProvided) ? 0 : _reserveIdBlock(opCtx, nRecords);

        // Non-clustered record stores will extract the RecordId key for the oplog and generate
        // unique int64_t RecordIds if RecordIds are not set.
        for (size_t i = 0; i < nRecords; i++) {
            auto& record = records[i];
            if (_isOplog) {
                auto swRecordId = record_id_helpers::keyForOptime(timestamps[i], KeyFormat::Long);
                if (!swRecordId.isOK())
                    return swRecordId.getStatus();

                // In the normal write paths, a timestamp is always set. It is only in unusual cases
                // like inserting the oplog seed document where the caller does not provide a
                // timestamp.
                if (MONGO_unlikely(timestamps[i].isNull() || kDebugBuild)) {
                    auto swRecordIdFromBSON =
                        record_id_helpers::extractKeyOptime(record.data.data(), record.data.size());
                    if (!swRecordIdFromBSON.isOK())
                        return swRecordIdFromBSON.getStatus();

                    // Double-check that the 'ts' field in the oplog entry matches the assigned
                    // timestamp, if it was provided.
                    dassert(timestamps[i].isNull() ||
                                swRecordIdFromBSON.getValue() == swRecordId.getValue(),
                            fmt::format(
                                "ts field in oplog entry {} does not equal assigned timestamp {}",
                                swRecordIdFromBSON.getValue().toString(),
                                swRecordId.getValue().toString()));

                    record.id = std::move(swRecordIdFromBSON.getValue());
                } else {
                    record.id = std::move(swRecordId.getValue());
                }
                // The records being inserted into the oplog must have increasing
                // recordId. Therefore the last record has the highest recordId.
                dassert(i == 0 || records[i].id > records[i - 1].id);
            } else {
                // Some RecordStores, like TemporaryRecordStores, may want to set their own
                // RecordIds.
                if (!areRecordIdsProvided) {
                    // Since a recordId wasn't provided for the first record, the recordId
                    // shouldn't have been provided for any record.
                    invariant(record.id.isNull());
                    record.id = RecordId(nextId++);
                    invariant(record.id.isValid());
                } else {
                    // Since a recordId was provided for the first record, the recordId
                    // should have been provided for all records.
                    invariant(!record.id.isNull());
                    if (record.id > highestRecordIdProvided) {
                        highestRecordIdProvided = record.id;
                    }
                }
            }
        }

        // Update the highest recordId we've seen so far on this record store, in case
        // any of the inserts we are performing has a higher recordId.
        // We only have to do this when the records we are inserting were accompanied
        // by caller provided recordIds.
        if (areRecordIdsProvided) {
            _updateLargestRecordId(opCtx, highestRecordIdProvided.getLong());
        }
    }

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);

    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++) {
        auto& record = records[i];
        totalLength += record.data.size();
        invariant(!record.id.isNull());
        invariant(!record_id_helpers::isReserved(record.id));
        Timestamp ts = timestamps[i];
        if (_isOplog) {
            // Setting this transaction to be unordered will trigger a journal flush. Because these
            // are direct writes into the oplog, the machinery to trigger a journal flush is
            // bypassed. A followup oplog read will require a fres value to make progress.
            shard_role_details::getRecoveryUnit(opCtx)->setOrderedCommit(false);
            auto oplogKeyTs = Timestamp(record.id.getLong());
            if (!ts.isNull()) {
                invariant(oplogKeyTs == ts);
            }
            if (!shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull()) {
                invariant(oplogKeyTs ==
                          shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp());
            }
        }
        if (!ts.isNull()) {
            LOGV2_DEBUG(22403, 4, "inserting record with timestamp {ts}", "ts"_attr = ts);
            fassert(39001, shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(ts));
        }
        CursorKey key = makeCursorKey(record.id, _keyFormat);
        setKey(c, &key);
        WiredTigerItem value(record.data.data(), record.data.size());
        c->set_value(c, value.Get());
        int ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(opCtx), c));

        if (ret == WT_DUPLICATE_KEY) {
            invariant(!_overwrite);
            invariant(_keyFormat == KeyFormat::String);

            BSONObj foundValueObj;
            if (TestingProctor::instance().isEnabled()) {
                WT_ITEM foundValue;
                invariantWTOK(c->get_value(c, &foundValue), c->session);
                foundValueObj = BSONObj(reinterpret_cast<const char*>(foundValue.data));
            }

            return Status{DuplicateKeyErrorInfo{BSONObj(),
                                                BSONObj(),
                                                BSONObj(),
                                                std::move(foundValueObj),
                                                std::move(record.id)},
                          "Duplicate cluster key found"};
        }

        if (ret)
            return wtRCToStatus(ret, c->session, "WiredTigerRecordStore::insertRecord");

        // Increment metrics for each insert separately, as opposed to outside of the loop. The API
        // requires that each record be accounted for separately.
        if (!_isOplog && !_isChangeCollection) {
            auto keyLength = computeRecordIdSize(record.id);
            metricsCollector.incrementOneDocWritten(_uri, value.size + keyLength);
        }
    }
    _changeNumRecordsAndDataSize(opCtx, nRecords, totalLength);

    std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
    if (_oplogTruncateMarkers) {
        invariant(_isOplog);
        // records[nRecords - 1] is the record in the oplog with the highest recordId.
        auto wall = [&] {
            BSONObj obj = records[nRecords - 1].data.toBson();
            BSONElement ele = obj[repl::DurableOplogEntry::kWallClockTimeFieldName];
            if (!ele) {
                // This shouldn't happen in normal cases, but this is needed because some tests do
                // not add wall clock times. Note that, with this addition, it's possible that the
                // oplog may grow larger than expected if --oplogMinRetentionHours is set.
                return Date_t::now();
            } else {
                return ele.Date();
            }
        }();
        _oplogTruncateMarkers->updateCurrentMarkerAfterInsertOnCommit(opCtx,
                                                                      totalLength,
                                                                      records[nRecords - 1].id,
                                                                      wall,
                                                                      nRecords,
                                                                      gOplogSamplingAsyncEnabled);
    }

    return Status::OK();
}

void WiredTigerRecordStore::sampleAndUpdate(OperationContext* opCtx) {
    auto oplogTruncateMarkers =
        OplogTruncateMarkers::sampleAndUpdate(opCtx, this, NamespaceString::kRsOplogNamespace);
    invariant(oplogTruncateMarkers);
    std::lock_guard lk(_oplogTruncateMarkersMutex);
    _oplogTruncateMarkers = oplogTruncateMarkers;
}

bool WiredTigerRecordStore::isOpHidden_forTest(const RecordId& id) const {
    invariant(_isOplog);
    invariant(id.getLong() > 0);
    invariant(_kvEngine->getOplogManager()->isRunning());
    return _kvEngine->getOplogManager()->getOplogReadTimestamp() <
        static_cast<std::uint64_t>(id.getLong());
}

StatusWith<Timestamp> WiredTigerRecordStore::getLatestOplogTimestamp(
    OperationContext* opCtx) const {
    invariant(_isOplog);
    invariant(_keyFormat == KeyFormat::Long);

    // Using this function inside a UOW is not supported because the main reason to call it is to
    // synchronize to the last op before waiting for write concern, so it makes little sense to do
    // so in a UOW. This also ensures we do not return uncommited entries.
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    auto wtRu = WiredTigerRecoveryUnit::get(opCtx);
    bool ruWasActive = wtRu->isActive();

    // getSession will open a txn if there was no txn active.
    auto session = wtRu->getSession();

    ON_BLOCK_EXIT([&] {
        if (!ruWasActive) {
            // In case the RU was inactive, leave it in that state.
            wtRu->abandonSnapshot();
        }
    });

    WT_CURSOR* cursor = writeConflictRetry(
        opCtx, "getLatestOplogTimestamp", NamespaceString::kRsOplogNamespace, [&] {
            auto cachedCursor = session->getCachedCursor(_tableId, "");
            return cachedCursor ? cachedCursor : session->getNewCursor(_uri);
        });
    ON_BLOCK_EXIT([&] { session->releaseCursor(_tableId, cursor, ""); });
    int ret = cursor->prev(cursor);
    if (ret == WT_NOTFOUND) {
        return Status(ErrorCodes::CollectionIsEmpty, "oplog is empty");
    }
    invariantWTOK(ret, cursor->session);

    RecordId recordId = getKey(cursor, _keyFormat);

    return {Timestamp(static_cast<unsigned long long>(recordId.getLong()))};
}

StatusWith<Timestamp> WiredTigerRecordStore::getEarliestOplogTimestamp(OperationContext* opCtx) {
    invariant(_isOplog);
    invariant(_keyFormat == KeyFormat::Long);
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());

    // Using relaxed loads is fine here. The returned timestamp can be from a deleted oplog entry by
    // the time we return from the method. Additionally we perform initialisation that uses strong
    // memory ordering so initialisation will only work if we've actually never initialised the
    // timestamp.
    auto firstRecordTimestamp = _oplogFirstRecordTimestamp.loadRelaxed();
    if (firstRecordTimestamp == Timestamp()) {
        WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
        auto sessRaii = cache->getSession();
        WT_CURSOR* cursor = writeConflictRetry(
            opCtx, "getEarliestOplogTimestamp", NamespaceString::kRsOplogNamespace, [&] {
                auto cachedCursor = sessRaii->getCachedCursor(_tableId, "");
                return cachedCursor ? cachedCursor : sessRaii->getNewCursor(_uri);
            });
        ON_BLOCK_EXIT([&] { sessRaii->releaseCursor(_tableId, cursor, ""); });
        auto ret = cursor->next(cursor);
        if (ret == WT_NOTFOUND) {
            return Status(ErrorCodes::CollectionIsEmpty, "oplog is empty");
        }
        invariantWTOK(ret, cursor->session);

        Timestamp ts{static_cast<uint64_t>(getKey(cursor, KeyFormat::Long).getLong())};
        if (_oplogFirstRecordTimestamp.compareAndSwap(&firstRecordTimestamp, ts)) {
            firstRecordTimestamp = ts;
        }
    }

    return firstRecordTimestamp;
}

Status WiredTigerRecordStore::doUpdateRecord(OperationContext* opCtx,
                                             const RecordId& id,
                                             const char* data,
                                             int len) {
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    WiredTigerCursor curwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    CursorKey key = makeCursorKey(id, _keyFormat);
    setKey(c, &key);
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return c->search(c); });

    invariantWTOK(ret,
                  c->session,
                  str::stream() << "Namespace: " << ns(opCtx).toStringForErrorMsg()
                                << "; Key: " << getKey(c, _keyFormat) << "; Read Timestamp: "
                                << shard_role_details::getRecoveryUnit(opCtx)
                                       ->getPointInTimeReadTimestamp(opCtx)
                                       .value_or(Timestamp{})
                                       .toString());

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret, c->session);

    int64_t old_length = old_value.size;

    {
        std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
        if (_oplogTruncateMarkers && len != old_length) {
            return {ErrorCodes::IllegalOperation,
                    "Cannot change the size of a document in the oplog"};
        }
    }

    WiredTigerItem value(data, len);

    // Check if we should modify rather than doing a full update.  Look for deltas for documents
    // larger than 1KB, up to 16 changes representing up to 10% of the data.
    //
    // Skip modify for logged tables: don't trust WiredTiger's recovery with operations that are not
    // idempotent.
    const int kMinLengthForDiff = 1024;
    const int kMaxEntries = 16;
    const int kMaxDiffBytes = len / 10;

    bool skip_update = false;
    if (!_forceUpdateWithFullDocument && !_isLogged && len > kMinLengthForDiff &&
        len <= old_length + kMaxDiffBytes) {
        int nentries = kMaxEntries;
        std::vector<WT_MODIFY> entries(nentries);

        if ((ret = wiredtiger_calc_modify(
                 c->session, &old_value, value.Get(), kMaxDiffBytes, entries.data(), &nentries)) ==
            0) {
            invariantWTOK(
                WT_OP_CHECK(
                    nentries == 0
                        ? c->reserve(c)
                        : wiredTigerCursorModify(
                              *WiredTigerRecoveryUnit::get(opCtx), c, entries.data(), nentries)),
                c->session);

            WT_ITEM new_value;
            dassert(nentries == 0 ||
                    (c->get_value(c, &new_value) == 0 && new_value.size == value.size &&
                     memcmp(data, new_value.data, len) == 0));
            skip_update = true;
        } else if (ret != WT_NOTFOUND) {
            invariantWTOK(ret, c->session);
        }
    }

    if (!skip_update) {
        c->set_value(c, value.Get());
        ret = WT_OP_CHECK(wiredTigerCursorInsert(*WiredTigerRecoveryUnit::get(opCtx), c));
    }
    invariantWTOK(ret, c->session);

    auto sizeDiff = len - old_length;

    // For updates that don't modify the document size, they should count as at least one unit, so
    // just attribute them as 1-byte modifications for simplicity.
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementOneDocWritten(_uri, std::max((int64_t)1, std::abs(sizeDiff)));
    _changeNumRecordsAndDataSize(opCtx, 0, sizeDiff);
    return Status::OK();
}

bool WiredTigerRecordStore::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> WiredTigerRecordStore::doUpdateWithDamages(
    OperationContext* opCtx,
    const RecordId& id,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {

    const int nentries = damages.size();
    mutablebson::DamageVector::const_iterator where = damages.begin();
    const mutablebson::DamageVector::const_iterator end = damages.cend();
    std::vector<WT_MODIFY> entries(nentries);
    for (u_int i = 0; where != end; ++i, ++where) {
        entries[i].data.data = damageSource + where->sourceOffset;
        entries[i].data.size = where->sourceSize;
        entries[i].offset = where->targetOffset;
        entries[i].size = where->targetSize;
    }

    WiredTigerCursor curwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    CursorKey key = makeCursorKey(id, _keyFormat);
    setKey(c, &key);

    // The test harness calls us with empty damage vectors which WiredTiger doesn't allow.
    if (nentries == 0)
        invariantWTOK(WT_OP_CHECK(c->search(c)), c->session);
    else
        invariantWTOK(WT_OP_CHECK(wiredTigerCursorModify(
                          *WiredTigerRecoveryUnit::get(opCtx), c, entries.data(), nentries)),
                      c->session);


    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value), c->session);

    auto sizeDiff = static_cast<int64_t>(value.size) - static_cast<int64_t>(oldRec.size());
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);

    // For updates that don't modify the document size, they should count as at least one unit, so
    // just attribute them as 1-byte modifications for simplicity.
    metricsCollector.incrementOneDocWritten(_uri, std::max((int64_t)1, std::abs(sizeDiff)));
    _changeNumRecordsAndDataSize(opCtx, 0, sizeDiff);

    return RecordData(static_cast<const char*>(value.data), value.size).getOwned();
}

void WiredTigerRecordStore::printRecordMetadata(OperationContext* opCtx,
                                                const RecordId& recordId,
                                                std::set<Timestamp>* recordTimestamps) const {
    // Printing the record metadata requires a new session. We cannot open other cursors when there
    // are open history store cursors in the session.
    WiredTigerSession session(_kvEngine->getConnection());

    // Per the version cursor API:
    // - A version cursor can only be called with the read timestamp as the oldest timestamp.
    // - If there is no oldest timestamp, the version cursor can only be called with a read
    //   timestamp of 1.
    Timestamp oldestTs = _kvEngine->getOldestTimestamp();
    const std::string config = "read_timestamp={:x},roundup_timestamps=(read=true)"_format(
        oldestTs.isNull() ? 1 : oldestTs.asULL());
    WiredTigerBeginTxnBlock beginTxn(&session, config.c_str());

    // Open a version cursor. This is a debug cursor that enables iteration through the history of
    // values for a given record.
    WT_CURSOR* cursor = session.getNewCursor(_uri, "debug=(dump_version=true)");

    CursorKey key = makeCursorKey(recordId, _keyFormat);
    setKey(cursor, &key);

    int ret = cursor->search(cursor);
    while (ret != WT_NOTFOUND) {
        invariantWTOK(ret, cursor->session);

        uint64_t startTs = 0, startDurableTs = 0, stopTs = 0, stopDurableTs = 0;
        uint64_t startTxnId = 0, stopTxnId = 0;
        uint8_t flags = 0, location = 0, prepare = 0, type = 0;
        WT_ITEM value;

        invariantWTOK(cursor->get_value(cursor,
                                        &startTxnId,
                                        &startTs,
                                        &startDurableTs,
                                        &stopTxnId,
                                        &stopTs,
                                        &stopDurableTs,
                                        &type,
                                        &prepare,
                                        &flags,
                                        &location,
                                        &value),
                      cursor->session);

        RecordData recordData(static_cast<const char*>(value.data), value.size);
        LOGV2(6120300,
              "WiredTiger record metadata",
              "recordId"_attr = recordId,
              "startTxnId"_attr = startTxnId,
              "startTs"_attr = Timestamp(startTs),
              "startDurableTs"_attr = Timestamp(startDurableTs),
              "stopTxnId"_attr = stopTxnId,
              "stopTs"_attr = Timestamp(stopTs),
              "stopDurableTs"_attr = Timestamp(stopDurableTs),
              "type"_attr = type,
              "prepare"_attr = prepare,
              "flags"_attr = flags,
              "location"_attr = location,
              "value"_attr = redact(recordData.toBson()));

        // Save all relevant timestamps that we just printed.
        if (recordTimestamps) {
            auto saveRecordTimestampIfValid = [recordTimestamps](Timestamp ts) {
                if (ts.isNull() || ts == Timestamp::max() || ts == Timestamp::min()) {
                    return;
                }
                (void)recordTimestamps->emplace(ts);
            };
            saveRecordTimestampIfValid(Timestamp(startTs));
            saveRecordTimestampIfValid(Timestamp(startDurableTs));
            saveRecordTimestampIfValid(Timestamp(stopTs));
            saveRecordTimestampIfValid(Timestamp(stopDurableTs));
        }

        ret = cursor->next(cursor);
    }
}

std::unique_ptr<RecordCursor> WiredTigerRecordStore::getRandomCursor(
    OperationContext* opCtx) const {
    return std::make_unique<RandomCursor>(opCtx, *this);
}

Status WiredTigerRecordStore::doTruncate(OperationContext* opCtx) {
    WiredTigerCursor startWrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    WT_CURSOR* start = startWrap.get();
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return start->next(start); });
    // Empty collections don't have anything to truncate.
    if (ret == WT_NOTFOUND) {
        return Status::OK();
    }
    invariantWTOK(ret, start->session);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    invariantWTOK(WT_OP_CHECK(session->truncate(session, nullptr, start, nullptr, nullptr)),
                  session);
    _changeNumRecordsAndDataSize(opCtx, -numRecords(opCtx), -dataSize(opCtx));

    std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
    if (_oplogTruncateMarkers) {
        _oplogTruncateMarkers->clearMarkersOnCommit(opCtx);
    }

    return Status::OK();
}

Status WiredTigerRecordStore::doRangeTruncate(OperationContext* opCtx,
                                              const RecordId& minRecordId,
                                              const RecordId& maxRecordId,
                                              int64_t hintDataSizeDiff,
                                              int64_t hintNumRecordsDiff) {
    WiredTigerCursor startWrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    WT_CURSOR* start = startWrap.get();
    int ret = wiredTigerPrepareConflictRetry(opCtx, [&] { return start->next(start); });
    // Empty collections don't have anything to truncate.
    if (ret == WT_NOTFOUND) {
        return Status::OK();
    }
    invariantWTOK(ret, start->session);
    // Make sure to reset the cursor since we have to replace it with what the user provided us.
    invariantWTOK(start->reset(start), start->session);

    boost::optional<CursorKey> startKey;
    if (minRecordId != RecordId()) {
        startKey = makeCursorKey(minRecordId, _keyFormat);
        setKey(start, &(*startKey));
    } else {
        start = nullptr;
    }
    WiredTigerCursor endWrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    boost::optional<CursorKey> endKey;
    WT_CURSOR* finish = [&]() -> WT_CURSOR* {
        if (maxRecordId == RecordId()) {
            return nullptr;
        }
        endKey = makeCursorKey(maxRecordId, _keyFormat);
        setKey(endWrap.get(), &(*endKey));
        return endWrap.get();
    }();

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    invariantWTOK(WT_OP_CHECK(session->truncate(session, nullptr, start, finish, nullptr)),
                  session);
    _changeNumRecordsAndDataSize(opCtx, hintNumRecordsDiff, hintDataSizeDiff);

    return Status::OK();
}

StatusWith<int64_t> WiredTigerRecordStore::doCompact(OperationContext* opCtx,
                                                     const CompactOptions& options) {
    dassert(shard_role_details::getLocker(opCtx)->isWriteLocked());

    WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
    if (cache->isEphemeral()) {
        return 0;
    }

    WT_SESSION* s = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    // Set a pointer on the WT_SESSION to the opCtx, so that WT::compact can use a callback to
    // check for interrupts.
    SessionDataRAII sessionRaii(s, opCtx);

    StringBuilder config;
    config << "timeout=0";
    if (options.dryRun) {
        config << ",dryrun=true";
    }
    if (options.freeSpaceTargetMB) {
        config << ",free_space_target=" << std::to_string(*options.freeSpaceTargetMB) << "MB";
    }
    const std::string uri(getURI());
    int ret = s->compact(s, uri.c_str(), config.str().c_str());
    if (ret == WT_ERROR && !opCtx->checkForInterruptNoAssert().isOK()) {
        return Status(ErrorCodes::Interrupted,
                      str::stream() << "Storage compaction interrupted on " << uri);
    }

    if (MONGO_unlikely(WTCompactRecordStoreEBUSY.shouldFail())) {
        ret = EBUSY;
    }

    if (ret == EBUSY) {
        return Status(ErrorCodes::Interrupted,
                      str::stream() << "Compaction interrupted on " << getURI());
    }
    invariantWTOK(ret, s);

    return options.dryRun ? WiredTigerUtil::getIdentCompactRewrittenExpectedSize(s, uri) : 0;
}

void WiredTigerRecordStore::validate(OperationContext* opCtx, bool full, ValidateResults* results) {
    dassert(shard_role_details::getLocker(opCtx)->isReadLocked());

    if (_isEphemeral) {
        return;
    }

    WiredTigerUtil::validateTableLogging(*WiredTigerRecoveryUnit::get(opCtx),
                                         _uri,
                                         _isLogged,
                                         boost::none,
                                         results->valid,
                                         results->errors,
                                         results->warnings);

    if (!full) {
        return;
    }

    if (_isOplog) {
        results->warnings.push_back("Skipping verification of the WiredTiger table for the oplog.");
        return;
    }

    int err =
        WiredTigerUtil::verifyTable(*WiredTigerRecoveryUnit::get(opCtx), _uri, &results->errors);
    if (!err) {
        return;
    }

    if (err == EBUSY) {
        std::string msg = str::stream()
            << "Could not complete validation of " << _uri << ". "
            << "This is a transient issue as the collection was actively "
               "in use by other operations.";

        LOGV2_WARNING(22408,
                      "Could not complete validation, This is a transient issue as the collection "
                      "was actively in use by other operations",
                      "uri"_attr = _uri);
        results->warnings.push_back(msg);
        return;
    }

    const char* errorStr = wiredtiger_strerror(err);
    std::string msg = str::stream() << "verify() returned " << errorStr << ". "
                                    << "This indicates structural damage. "
                                    << "Not examining individual documents.";
    LOGV2_ERROR(22409,
                "Verification returned error. This indicates structural damage. Not examining "
                "individual documents",
                "error"_attr = errorStr);
    results->errors.push_back(msg);
    results->valid = false;
}

void WiredTigerRecordStore::appendNumericCustomStats(OperationContext* opCtx,
                                                     BSONObjBuilder* result,
                                                     double scale) const {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    WT_SESSION* s = session->getSession();

    BSONObjBuilder bob(result->subobjStart(_engineName));

    appendNumericStats(s, getURI(), bob);
}

void WiredTigerRecordStore::appendAllCustomStats(OperationContext* opCtx,
                                                 BSONObjBuilder* result,
                                                 double scale) const {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSessionNoTxn();
    WT_SESSION* s = session->getSession();
    BSONObjBuilder bob(result->subobjStart(_engineName));
    {
        BSONObjBuilder metadata(bob.subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(
            *WiredTigerRecoveryUnit::get(opCtx), getURI(), &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }

    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(
        *WiredTigerRecoveryUnit::get(opCtx), _uri, &type, &sourceURI);
    StatusWith<std::string> metadataResult =
        WiredTigerUtil::getMetadataCreate(*WiredTigerRecoveryUnit::get(opCtx), sourceURI);
    StringData creationStringName("creationString");
    if (!metadataResult.isOK()) {
        BSONObjBuilder creationString(bob.subobjStart(creationStringName));
        creationString.append("error", "unable to retrieve creation config");
        creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
        creationString.append("reason", metadataResult.getStatus().reason());
    } else {
        bob.append("creationString", metadataResult.getValue());
        // Type can be "lsm" or "file"
        bob.append("type", type);
    }

    appendNumericStats(s, getURI(), bob);
}

void WiredTigerRecordStore::waitForAllEarlierOplogWritesToBeVisibleImpl(
    OperationContext* opCtx) const {
    // Make sure that callers do not hold an active snapshot so it will be able to see the oplog
    // entries it waited for afterwards.
    if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
        shard_role_details::getLocker(opCtx)->dump();
        invariant(!shard_role_details::getRecoveryUnit(opCtx)->isActive(),
                  str::stream() << "Unexpected open storage txn. RecoveryUnit state: "
                                << RecoveryUnit::toString(
                                       shard_role_details::getRecoveryUnit(opCtx)->getState())
                                << ", inMultiDocumentTransaction:"
                                << (opCtx->inMultiDocumentTransaction() ? "true" : "false"));
    }

    auto oplogManager = _kvEngine->getOplogManager();
    if (oplogManager->isRunning()) {
        oplogManager->waitForAllEarlierOplogWritesToBeVisible(this, opCtx);
    }
}

void WiredTigerRecordStore::updateStatsAfterRepair(OperationContext* opCtx,
                                                   long long numRecords,
                                                   long long dataSize) {
    // We're correcting the size as of now, future writes should be tracked.
    sizeRecoveryState(getGlobalServiceContext())
        .markCollectionAsAlwaysNeedsSizeAdjustment(getIdent());

    _sizeInfo->numRecords.store(std::max(numRecords, 0ll));
    _sizeInfo->dataSize.store(std::max(dataSize, 0ll));

    // If we have a WiredTigerSizeStorer, but our size info is not currently cached, add it.
    if (_sizeStorer)
        _sizeStorer->store(_uri, _sizeInfo);
}

void WiredTigerRecordStore::_initNextIdIfNeeded(OperationContext* opCtx) {
    // Clustered record stores do not automatically generate int64 RecordIds. RecordIds are instead
    // constructed as binary strings, KeyFormat::String, from the user-defined cluster key.
    invariant(_keyFormat == KeyFormat::Long);

    // In the normal case, this will already be initialized, so use a weak load. Since this value
    // will only change from 0 to a positive integer, the only risk is reading an outdated value, 0,
    // and having to take the mutex.
    if (_nextIdNum.loadRelaxed() > 0) {
        return;
    }

    // Only one thread needs to do this.
    stdx::lock_guard<Latch> lk(_initNextIdMutex);
    if (_nextIdNum.load() > 0) {
        return;
    }

    // During startup recovery, the collectionAlwaysNeedsSizeAdjustment flag is not set by default
    // for the sake of efficiency. However, if we reach this point, we may need to set it in order
    // to ensure that capped deletes can occur on documents inserted earlier in startup recovery.
    if (inReplicationRecovery(opCtx->getServiceContext()).load() &&
        !sizeRecoveryState(opCtx->getServiceContext())
             .collectionAlwaysNeedsSizeAdjustment(getIdent())) {
        checkSize(opCtx);
    }

    // Find the largest RecordId in the table and add 1 to generate our next RecordId. The
    // largest_key API returns the largest key in the table regardless of visibility. This ensures
    // we don't re-use RecordIds that are not visible.

    // Need to start at 1 so we are always higher than RecordId::minLong(). This will be the case if
    // the table is empty, and returned RecordId is null.
    int64_t nextId = getLargestKey(opCtx).getLong() + 1;
    _nextIdNum.store(nextId);
}

RecordId WiredTigerRecordStore::getLargestKey(OperationContext* opCtx) const {
    // Initialize the highest seen RecordId in a session without a read timestamp because that is
    // required by the largest_key API.
    WiredTigerSession sessRaii(_kvEngine->getConnection());
    auto wtSession = sessRaii.getSession();

    if (shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork()) {
        // We must limit the amount of time spent blocked on cache eviction to avoid a deadlock with
        // ourselves. The calling operation may have a session open that has written a large amount
        // of data, and by creating a new session, we are preventing WT from being able to roll back
        // that transaction to free up cache space. If we do block on cache eviction here, we must
        // consider that the other session owned by this thread may be the one that needs to be
        // rolled back. If this does time out, we will receive a WT_ROLLBACK and throw an error.
        invariantWTOK(wtSession->reconfigure(wtSession, "cache_max_wait_ms=1000"), wtSession);
    }

    auto cursor = sessRaii.getNewCursor(_uri);
    int ret = cursor->largest_key(cursor);
    if (ret == WT_ROLLBACK) {
        // Force the caller to rollback its transaction if we can't make progess with eviction.
        // TODO (SERVER-63620): Convert this to a different error code that is distinguishable from
        // a true write conflict.
        auto rollbackReason = wtSession->get_rollback_reason(wtSession);
        rollbackReason = rollbackReason ? rollbackReason : "undefined";
        throwWriteConflictException(
            fmt::format("Rollback ocurred while performing initial write to '{}'. Reason: '{}'",
                        ns(opCtx).toStringForErrorMsg(),
                        rollbackReason));
    } else if (ret != WT_NOTFOUND) {
        if (ret == ENOTSUP) {
            auto creationMetadata = WiredTigerUtil::getMetadataCreate(wtSession, _uri).getValue();
            if (creationMetadata.find("lsm=") != std::string::npos) {
                LOGV2_FATAL(
                    6627200,
                    "WiredTiger tables using 'type=lsm' (Log-Structured Merge Tree) are not "
                    "supported.",
                    "namespace"_attr = ns(opCtx),
                    "metadata"_attr = redact(creationMetadata));
            }
        }
        invariantWTOK(ret, wtSession);
        return getKey(cursor, _keyFormat);
    }
    // Empty table.
    return RecordId();
}

void WiredTigerRecordStore::reserveRecordIds(OperationContext* opCtx,
                                             std::vector<RecordId>* out,
                                             size_t nRecords) {
    auto nextId = _reserveIdBlock(opCtx, nRecords);
    for (size_t i = 0; i < nRecords; i++) {
        out->push_back(RecordId(nextId++));
    }
}

long long WiredTigerRecordStore::_reserveIdBlock(OperationContext* opCtx, size_t nRecords) {
    // Clustered record stores do not automatically generate int64 RecordIds. RecordIds are instead
    // constructed as binary strings, KeyFormat::String, from the user-defined cluster key.
    invariant(_keyFormat == KeyFormat::Long);
    invariant(!_isOplog);
    _initNextIdIfNeeded(opCtx);
    return _nextIdNum.fetchAndAdd(nRecords);
}

void WiredTigerRecordStore::_changeNumRecordsAndDataSize(OperationContext* opCtx,
                                                         int64_t numRecordDiff,
                                                         int64_t dataSizeDiff) {
    if (numRecordDiff == 0 && dataSizeDiff == 0) {
        // If there's nothing to increment/decrement this will be a no-op. Avoid all the other
        // checks and early return.
        return;
    }

    if (!_tracksSizeAdjustments) {
        return;
    }

    if (!sizeRecoveryState(getGlobalServiceContext()).collectionNeedsSizeAdjustment(getIdent())) {
        return;
    }

    const auto updateAndStoreSizeInfo = [this](int64_t numRecordDiff, int64_t dataSizeDiff) {
        _sizeInfo->numRecords.addAndFetch(numRecordDiff);
        _sizeInfo->dataSize.addAndFetch(dataSizeDiff);

        if (_sizeStorer)
            _sizeStorer->store(_uri, _sizeInfo);
    };

    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [updateAndStoreSizeInfo, numRecordDiff, dataSizeDiff](OperationContext*) {
            LOGV2_DEBUG(7105300,
                        3,
                        "WiredTigerRecordStore: rolling back change to numRecords and dataSize",
                        "numRecordDiff"_attr = -numRecordDiff,
                        "dataSizeDiff"_attr = -dataSizeDiff);
            updateAndStoreSizeInfo(-numRecordDiff, -dataSizeDiff);
        });
    updateAndStoreSizeInfo(numRecordDiff, dataSizeDiff);
}

void WiredTigerRecordStore::setNumRecords(long long numRecords) {
    _sizeInfo->numRecords.store(std::max(numRecords, 0ll));

    if (!_sizeStorer) {
        return;
    }

    // Flush the updated number of records to disk immediately.
    _sizeStorer->store(_uri, _sizeInfo);
    bool syncToDisk = true;
    _sizeStorer->flush(syncToDisk);
}

void WiredTigerRecordStore::setDataSize(long long dataSize) {
    _sizeInfo->dataSize.store(std::max(dataSize, 0ll));

    if (!_sizeStorer) {
        return;
    }

    // Flush the updated data size to disk immediately.
    _sizeStorer->store(_uri, _sizeInfo);
    bool syncToDisk = true;
    _sizeStorer->flush(syncToDisk);
}

void WiredTigerRecordStore::doCappedTruncateAfter(
    OperationContext* opCtx,
    const RecordId& end,
    bool inclusive,
    const AboutToDeleteRecordCallback& aboutToDelete) {
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, true);

    auto record = cursor->seekExact(end);
    massert(28807, str::stream() << "Failed to seek to the record located at " << end, record);

    int64_t recordsRemoved = 0;
    int64_t bytesRemoved = 0;
    RecordId lastKeptId;
    RecordId firstRemovedId;

    if (inclusive) {
        std::unique_ptr<SeekableRecordCursor> reverseCursor = getCursor(opCtx, false);
        invariant(reverseCursor->seekExact(end));
        auto prev = reverseCursor->next();
        lastKeptId = prev ? std::move(prev->id) : RecordId();
        firstRemovedId = end;
    } else {
        // If not deleting the record located at 'end', then advance the cursor to the first record
        // that is being deleted.
        record = cursor->next();
        if (!record) {
            return;  // No records to delete.
        }
        lastKeptId = end;
        firstRemovedId = record->id;
    }

    WriteUnitOfWork wuow(opCtx);

    // Compute the number and associated sizes of the records to delete.
    {
        do {
            if (aboutToDelete) {
                aboutToDelete(opCtx, record->id, record->data);
            }
            recordsRemoved++;
            bytesRemoved += record->data.size();
        } while ((record = cursor->next()));
    }

    // Truncate the collection starting from the record located at 'firstRemovedId' to the end of
    // the collection.

    WiredTigerCursor startwrap(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    WT_CURSOR* start = startwrap.get();
    CursorKey key = makeCursorKey(firstRemovedId, _keyFormat);
    setKey(start, &key);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    invariantWTOK(session->truncate(session, nullptr, start, nullptr, nullptr), session);

    _changeNumRecordsAndDataSize(opCtx, -recordsRemoved, -bytesRemoved);

    wuow.commit();

    if (_isOplog) {
        // Immediately rewind visibility to our truncation point, to prevent new
        // transactions from appearing.
        Timestamp truncTs(lastKeptId.getLong());

        if (!serverGlobalParams.enableMajorityReadConcern &&
            _kvEngine->getOldestTimestamp() > truncTs) {
            // If majority read concern is disabled, the oldest timestamp can be ahead of 'truncTs'.
            // In that case, we must set the oldest timestamp along with the commit timestamp.
            // Otherwise, the commit timestamp will be set behind the oldest timestamp, which is
            // illegal.
            const bool force = true;
            _kvEngine->setOldestTimestamp(truncTs, force);
        } else {
            auto conn = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache()->conn();
            auto durableTSConfigString = "durable_timestamp={:x}"_format(truncTs.asULL());
            invariantWTOK(conn->set_timestamp(conn, durableTSConfigString.c_str()), session);
        }

        _kvEngine->getOplogManager()->setOplogReadTimestamp(truncTs);
        LOGV2_DEBUG(22405, 1, "truncation new read timestamp: {truncTs}", "truncTs"_attr = truncTs);
    }

    std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
    if (_oplogTruncateMarkers) {
        _oplogTruncateMarkers->updateMarkersAfterCappedTruncateAfter(
            recordsRemoved, bytesRemoved, firstRemovedId);
    }
}

Status WiredTigerRecordStore::oplogDiskLocRegisterImpl(OperationContext* opCtx,
                                                       const Timestamp& ts,
                                                       bool orderedCommit) {
    shard_role_details::getRecoveryUnit(opCtx)->setOrderedCommit(orderedCommit);

    if (!orderedCommit) {
        // This labels the current transaction with a timestamp.
        // This is required for oplog visibility to work correctly, as WiredTiger uses the
        // transaction list to determine where there are holes in the oplog.
        return shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(ts);
    }

    // This handles non-primary (secondary) state behavior; we simply set the oplog visiblity read
    // timestamp here, as there cannot be visible holes prior to the opTime passed in.
    _kvEngine->getOplogManager()->setOplogReadTimestamp(ts);

    // Inserts and updates usually notify waiters on commit, but the oplog collection has special
    // visibility rules and waiters must be notified whenever the oplog read timestamp is forwarded.
    notifyCappedWaitersIfNeeded();

    return Status::OK();
}

std::unique_ptr<SeekableRecordCursor> WiredTigerRecordStore::getCursor(OperationContext* opCtx,
                                                                       bool forward) const {
    if (_isOplog) {
        return std::make_unique<WiredTigerOplogCursor>(opCtx, *this, forward);
    } else if (_isCapped) {
        return std::make_unique<WiredTigerStandardCappedCursor>(opCtx, *this, forward);
    }
    return std::make_unique<WiredTigerRecordStoreCursor>(opCtx, *this, forward);
}

Status WiredTigerRecordStore::updateOplogSize(OperationContext* opCtx, long long newOplogSize) {
    invariant(_isOplog && _oplogMaxSize.load());

    if (_oplogMaxSize.load() == newOplogSize) {
        return Status::OK();
    }

    _oplogMaxSize.store(newOplogSize);

    std::shared_lock lk(_oplogTruncateMarkersMutex);  // NOLINT
    invariant(_oplogTruncateMarkers);
    _oplogTruncateMarkers->adjust(opCtx, newOplogSize);
    return Status::OK();
}

WiredTigerRecordStoreCursor::WiredTigerRecordStoreCursor(OperationContext* opCtx,
                                                         const WiredTigerRecordStore& rs,
                                                         bool forward)
    : _tableId(rs.tableId()),
      _opCtx(opCtx),
      _uri(rs.getURI()),
      _ident(rs.getIdent()),
      _keyFormat(rs.keyFormat()),
      _forward(forward),
      _uuid(rs.uuid()),
      _assertOutOfOrderForTest(MONGO_unlikely(WTRecordStoreUassertOutOfOrder.shouldFail())) {
    _cursor.emplace(*WiredTigerRecoveryUnit::get(opCtx), _uri, _tableId, true);
    auto metrics = &ResourceConsumption::MetricsCollector::get(opCtx);

    // Assumption: cursors are always scoped within the context of a scoped metrics collector.
    if (metrics->isCollecting()) {
        _metrics = metrics;
    }
}

boost::optional<Record> WiredTigerRecordStoreCursor::next() {
    auto id = nextIdCommon();
    if (id.isNull()) {
        return boost::none;
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    checkOrder(toReturn.id);
    trackReturn(toReturn);
    return toReturn;
}

RecordId WiredTigerRecordStoreCursor::nextIdCommon() {
    invariant(_hasRestored);
    if (_eof)
        return {};

    // Ensure an active transaction is open. While WiredTiger supports using cursors on a session
    // without an active transaction (i.e. an implicit transaction), that would bypass configuration
    // options we pass when we explicitly start transactions in the RecoveryUnit.
    WiredTigerRecoveryUnit::get(_opCtx)->getSession();

    WT_CURSOR* c = _cursor->get();

    RecordId id;
    if (!_skipNextAdvance) {
        // Nothing after the next line can throw WCEs.
        // Note that an unpositioned (or eof) WT_CURSOR returns the first/last entry in the
        // table when you call next/prev.
        int advanceRet = wiredTigerPrepareConflictRetry(
            _opCtx, [&] { return _forward ? c->next(c) : c->prev(c); });
        if (advanceRet == WT_NOTFOUND) {
            _eof = true;
            _positioned = false;
            return {};
        }
        invariantWTOK(advanceRet, c->session);
        id = getKey(c, _keyFormat);
    } else if (!id.isValid()) {
        id = getKey(c, _keyFormat);
    }

    _positioned = true;
    _skipNextAdvance = false;
    return id;
}

void WiredTigerRecordStoreCursor::reportOutOfOrderRead(const RecordId& id,
                                                       bool failWithOutOfOrderForTest) const {
    HealthLogEntry entry;
    entry.setNss(namespaceForUUID(_opCtx, _uuid));
    entry.setTimestamp(Date_t::now());
    entry.setSeverity(SeverityEnum::Error);
    entry.setScope(ScopeEnum::Collection);
    entry.setOperation("WT_Cursor::next");
    entry.setMsg("Cursor returned out-of-order keys");

    BSONObjBuilder bob;
    bob.append("forward", _forward);
    bob.append("next", id.toString());
    bob.append("last", _lastReturnedId.toString());
    bob.append("ident", _ident);
    bob.appendElements(getStackTrace().getBSONRepresentation());
    entry.setData(bob.obj());

    HealthLogInterface::get(_opCtx)->log(entry);

    if (!failWithOutOfOrderForTest) {
        // Crash when testing diagnostics are enabled and not explicitly uasserting on
        // out-of-order keys.
        invariant(!TestingProctor::instance().isEnabled(), "cursor returned out-of-order keys");
    }

    auto options = [&] {
        if (shard_role_details::getRecoveryUnit(_opCtx)->getDataCorruptionDetectionMode() ==
            DataCorruptionDetectionMode::kThrow) {
            // uassert with 'DataCorruptionDetected' after logging.
            return logv2::LogOptions{logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)};
        } else {
            return logv2::LogOptions(logv2::LogComponent::kAutomaticDetermination);
        }
    }();

    LOGV2_ERROR_OPTIONS(22406,
                        options,
                        "WT_Cursor::next -- returned out-of-order keys",
                        "forward"_attr = _forward,
                        "next"_attr = id,
                        "last"_attr = _lastReturnedId,
                        "ident"_attr = _ident,
                        "ns"_attr = namespaceForUUID(_opCtx, _uuid));
}

void WiredTigerRecordStoreCursor::checkOrder(const RecordId& id) const {
    if (MONGO_unlikely((_forward && _lastReturnedId >= id) ||
                       (!_forward && !_lastReturnedId.isNull() && id >= _lastReturnedId) ||
                       _assertOutOfOrderForTest)) {
        reportOutOfOrderRead(id, _assertOutOfOrderForTest);
    }
}

void WiredTigerRecordStoreCursor::trackReturn(const Record& record) {
    if (_metrics) {
        _metrics->incrementOneDocRead(_uri, record.data.size() + computeRecordIdSize(record.id));
    }
    _lastReturnedId = record.id;
}

void WiredTigerRecordStoreCursor::resetCursor() {
    if (_cursor) {
        WT_CURSOR* c = _cursor->get();
        invariantWTOK(WT_READ_CHECK(c->reset(c)), c->session);
        _boundSet = false;
        _positioned = false;
    }
}

boost::optional<Record> WiredTigerRecordStoreCursor::seek(const RecordId& start,
                                                          BoundInclusion boundInclusion) {
    auto id = seekIdCommon(start, boundInclusion);
    if (id.isNull()) {
        return boost::none;
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    trackReturn(toReturn);
    return toReturn;
}

RecordId WiredTigerRecordStoreCursor::seekIdCommon(const RecordId& start,
                                                   BoundInclusion boundInclusion,
                                                   bool restoring) {
    invariant(_hasRestored || restoring);
    dassert(shard_role_details::getLocker(_opCtx)->isReadLocked());

    // Ensure an active transaction is open.
    auto session = WiredTigerRecoveryUnit::get(_opCtx)->getSession();
    _skipNextAdvance = false;

    // If the cursor is positioned, we need to reset it so that we can set bounds. This is not the
    // common use case.
    if (_positioned) {
        resetCursor();
    }

    WT_CURSOR* c = _cursor->get();
    WiredTigerRecordStore::CursorKey key = makeCursorKey(start, _keyFormat);
    setKey(c, &key);

    auto const& config = _forward
        ? (boundInclusion == BoundInclusion::kInclude ? lowerInclusiveBoundConfig
                                                      : lowerExclusiveBoundConfig)
        : (boundInclusion == BoundInclusion::kInclude ? upperInclusiveBoundConfig
                                                      : upperExclusiveBoundConfig);

    invariantWTOK(c->bound(c, config.getConfig(session)), c->session);
    _boundSet = true;

    int ret =
        wiredTigerPrepareConflictRetry(_opCtx, [&] { return _forward ? c->next(c) : c->prev(c); });
    if (ret == WT_NOTFOUND) {
        _eof = true;
        return {};
    }
    invariantWTOK(ret, c->session);

    _positioned = true;
    _eof = false;
    return getKey(c, _keyFormat);
}

boost::optional<Record> WiredTigerRecordStoreCursor::seekExact(const RecordId& id) {
    return seekExactCommon(id);
}

boost::optional<Record> WiredTigerRecordStoreCursor::seekExactCommon(const RecordId& id) {
    invariant(_hasRestored);

    // Ensure an active transaction is open. While WiredTiger supports using cursors on a session
    // without an active transaction (i.e. an implicit transaction), that would bypass configuration
    // options we pass when we explicitly start transactions in the RecoveryUnit.
    auto session = WiredTigerRecoveryUnit::get(_opCtx)->getSession();

    _skipNextAdvance = false;
    WT_CURSOR* c = _cursor->get();

    // Before calling WT search, clear any saved bounds from a previous seek.
    if (_boundSet) {
        invariantWTOK(c->bound(c, clearBoundConfig.getConfig(session)), c->session);
        _boundSet = false;
    }

    auto key = makeCursorKey(id, _keyFormat);
    setKey(c, &key);
    // Nothing after the next line can throw WCEs.
    int seekRet = wiredTigerPrepareConflictRetry(_opCtx, [&] { return c->search(c); });
    if (seekRet == WT_NOTFOUND) {
        _eof = true;
        return {};
    }
    invariantWTOK(seekRet, c->session);

    _eof = false;
    _positioned = true;
    Record toReturn = {id, getRecordData(c)};
    trackReturn(toReturn);
    return toReturn;
}

void WiredTigerRecordStoreCursor::save() {
    resetCursor();
    _hasRestored = false;
}

void WiredTigerRecordStoreCursor::saveUnpositioned() {
    save();
    _lastReturnedId = RecordId();
}

bool WiredTigerRecordStoreCursor::restore(bool tolerateCappedRepositioning) {
    if (!_cursor)
        _cursor.emplace(*WiredTigerRecoveryUnit::get(_opCtx), _uri, _tableId, true);

    // This will ensure an active session exists, so any restored cursors will bind to it
    invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());

    // If we've hit EOF, then this iterator is done and need not be restored.
    if (_eof || _lastReturnedId.isNull()) {
        _hasRestored = true;
        return true;
    }

    auto foundId = seekIdCommon(_lastReturnedId, BoundInclusion::kInclude, true /* restoring */);
    _hasRestored = true;
    if (foundId.isNull()) {
        _eof = true;
        return true;
    }

    int cmp = foundId.compare(_lastReturnedId);
    if (cmp == 0) {
        return true;  // Landed right where we left off.
    }

    // With bounded cursors, we should always find a key greater than the one we searched for.
    dassert(_forward ? cmp > 0 : cmp < 0);

    // We landed after where we were. Return our new location on the next call to next().
    _skipNextAdvance = true;
    return true;
}

void WiredTigerRecordStoreCursor::detachFromOperationContext() {
    _opCtx = nullptr;
    _metrics = nullptr;
    if (!_saveStorageCursorOnDetachFromOperationContext) {
        _cursor = boost::none;
    }
}

void WiredTigerRecordStoreCursor::reattachToOperationContext(OperationContext* opCtx) {
    _opCtx = opCtx;
    auto metrics = &ResourceConsumption::MetricsCollector::get(opCtx);
    if (metrics->isCollecting()) {
        _metrics = metrics;
    }
    // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
}

WiredTigerCappedCursorBase::WiredTigerCappedCursorBase(OperationContext* opCtx,
                                                       const WiredTigerRecordStore& rs,
                                                       bool forward)
    : WiredTigerRecordStoreCursor(opCtx, rs, forward) {}

boost::optional<Record> WiredTigerCappedCursorBase::seekExact(const RecordId& id) {
    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    return seekExactCommon(id);
}

boost::optional<Record> WiredTigerCappedCursorBase::seek(const RecordId& start,
                                                         BoundInclusion boundInclusion) {
    auto id = seekIdCommon(start, boundInclusion);
    if (id.isNull()) {
        return boost::none;
    }

    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    trackReturn(toReturn);
    return toReturn;
}

boost::optional<Record> WiredTigerCappedCursorBase::next() {
    auto id = nextIdCommon();
    if (id.isNull()) {
        return boost::none;
    }

    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    checkOrder(toReturn.id);
    trackReturn(toReturn);
    return toReturn;
}

void WiredTigerCappedCursorBase::save() {
    WiredTigerRecordStoreCursor::save();
    resetVisibility();
}

bool WiredTigerCappedCursorBase::restore(bool tolerateCappedRepositioning) {
    if (!_cursor) {
        _cursor.emplace(*WiredTigerRecoveryUnit::get(_opCtx), _uri, _tableId, true);
    }

    initVisibility(_opCtx);

    // This will ensure an active session exists, so any restored cursors will bind to it
    invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());

    // If we've hit EOF, then this iterator is done and need not be restored.
    if (_eof || _lastReturnedId.isNull()) {
        _hasRestored = true;
        return true;
    }

    auto foundId = seekIdCommon(_lastReturnedId, BoundInclusion::kInclude, true /* restoring */);
    _hasRestored = true;
    if (foundId.isNull()) {
        _eof = true;
        // Capped read collscans do not tolerate cursor repositioning. By contrast, write collscans
        // on a clustered collection like TTL deletion tolerate cursor repositioning like normal
        // collections.
        if (!tolerateCappedRepositioning) {
            return false;
        }
        return true;
    }

    int cmp = foundId.compare(_lastReturnedId);
    if (cmp == 0) {
        return true;  // Landed right where we left off.
    }

    if (!tolerateCappedRepositioning) {
        // The cursor has been repositioned as it was sitting on a document that has been
        // removed by capped collection deletion. It is important that we error out in this case
        // so that consumers don't silently get 'holes' when scanning capped collections.
        // We don't make this guarantee for normal collections or for write operations like
        // capped TTL deletion so it is ok to skip ahead in that case.
        _eof = true;
        return false;
    }

    // With bounded cursors, we should always find a key greater than the one we searched for.
    dassert(_forward ? cmp > 0 : cmp < 0);

    // We landed after where we were. Return our new location on the next call to next().
    _skipNextAdvance = true;
    return true;
}

WiredTigerStandardCappedCursor::WiredTigerStandardCappedCursor(OperationContext* opCtx,
                                                               const WiredTigerRecordStore& rs,
                                                               bool forward)
    : WiredTigerCappedCursorBase(opCtx, rs, forward) {
    initVisibility(opCtx);
}

bool WiredTigerStandardCappedCursor::isVisible(const RecordId& id) {
    if (!_forward) {
        return true;
    }
    if (_cappedSnapshot && !_cappedSnapshot->isRecordVisible(id)) {
        return false;
    }
    return true;
}

void WiredTigerStandardCappedCursor::initVisibility(OperationContext* opCtx) {
    if (_forward) {
        // We can't enforce that the caller has initialized the capped snapshot before entering this
        // function because we need to know, for example, what locks are held. So we expect higher
        // layers to do so.
        _cappedSnapshot = CappedSnapshots::get(_opCtx).getSnapshot(_ident);
    }
}

void WiredTigerStandardCappedCursor::resetVisibility() {
    _cappedSnapshot = boost::none;
}

WiredTigerOplogCursor::WiredTigerOplogCursor(OperationContext* opCtx,
                                             const WiredTigerRecordStore& rs,
                                             bool forward)
    : WiredTigerCappedCursorBase(opCtx, rs, forward) {
    initVisibility(opCtx);
}

void WiredTigerOplogCursor::initVisibility(OperationContext* opCtx) {
    auto wtRu = WiredTigerRecoveryUnit::get(opCtx);
    if (_forward) {
        _oplogVisibleTs = wtRu->getOplogVisibilityTs();
    }
    boost::optional<Timestamp> readTs = wtRu->getPointInTimeReadTimestamp(opCtx);
    if (readTs && readTs->asLL() != 0) {
        // One cannot pass a read_timestamp of 0 to WT, but a "0" is commonly understood as
        // every time is visible.
        _readTimestampForOplog = readTs->asInt64();
    }
}

bool WiredTigerOplogCursor::isVisible(const RecordId& id) {
    if (_readTimestampForOplog && id.getLong() > *_readTimestampForOplog) {
        return false;
    }
    if (!_forward) {
        return true;
    }
    if (_oplogVisibleTs && id.getLong() > *_oplogVisibleTs) {
        return false;
    }
    return true;
}

void WiredTigerOplogCursor::resetVisibility() {
    _oplogVisibleTs = boost::none;
    _readTimestampForOplog = boost::none;
}

boost::optional<Record> WiredTigerOplogCursor::next() {
    auto id = nextIdCommon();
    if (id.isNull()) {
        return boost::none;
    }

    auto cur = _cursor->get();

    // If we're using a read timestamp and we're a reverse cursor positioned outside of that bound,
    // walk backwards until we find a suitable record. This is exercised when doing a reverse
    // natural order collection scan.
    if (_readTimestampForOplog && !_forward) {
        while (id.getLong() > *_readTimestampForOplog) {
            int advanceRet = wiredTigerPrepareConflictRetry(_opCtx, [&] { return cur->prev(cur); });
            if (advanceRet == WT_NOTFOUND) {
                _positioned = false;
                _eof = true;
                return {};
            }
            invariantWTOK(advanceRet, cur->session);
            id = getKey(cur, _keyFormat);
        }
    }

    if (!isVisible(id)) {
        _eof = true;
        return {};
    }

    Record toReturn = {std::move(id), getRecordData(cur)};
    checkOrder(toReturn.id);
    trackReturn(toReturn);
    return toReturn;
}

boost::optional<Record> WiredTigerOplogCursor::seek(const RecordId& start,
                                                    BoundInclusion boundInclusion) {
    RecordId id;
    if (!_forward && _readTimestampForOplog && start.getLong() > *_readTimestampForOplog) {
        auto key = RecordId(*_readTimestampForOplog);
        id = seekIdCommon(key, BoundInclusion::kInclude);
    } else {
        id = seekIdCommon(start, boundInclusion);
    }

    if (id.isNull()) {
        return boost::none;
    }

    if (!isVisible(id)) {
        _eof = true;
        return boost::none;
    }

    Record toReturn = {std::move(id), getRecordData(_cursor->get())};
    trackReturn(toReturn);
    return toReturn;
}

}  // namespace mongo

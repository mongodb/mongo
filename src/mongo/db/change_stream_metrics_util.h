// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/base/error_codes.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
namespace mongo::change_stream {

// Construct options in each factory so static metric registration does not depend on
// dynamically initialized inline options being initialized first by the Windows cross toolchain.

// Constructs the counter for the OTEL metric
// "serverStatus.metrics.changeStreams.cursor.totalOpened".
inline otel::metrics::Counter<int64_t>& createCurorsTotalOpened() {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.totalOpened",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return otel::metrics::MetricsService::instance().createInt64Counter(
        otel::metrics::MetricNames::kChangeStreamCursorsTotalOpened,
        "Total number of change stream cursors opened (on router or shard).",
        otel::metrics::MetricUnit::kCursors,
        opts);
}

// Constructs the histogram for the OTEL metric
// "serverStatus.metrics.changeStreams.cursor.lifespan". The change stream lifespan histogram is
// updated after a change stream cursor is closed. A histogram provides accurate and thread-safe
// average for every bucket. This is achieved by locks, so there might be some overhead.
inline otel::metrics::Histogram<int64_t>& createCursorsLifespan() {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.lifespan",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    // Using the same histogram buckets as 'metrics.cursor.lifespan'. For change stream cursors we
    // expect that most of the cursors will land in (10min, +inf) bucket.
    opts.explicitBucketBoundaries = std::vector<double>({
        1 * 1e6,        // lifespan <= 1 second will probably capture one-fetch or no-fetch cursors,
                        // unless the query is slow for some reason
        10 * 1e6,       // lifespan <= 10 seconds will probably capture other 'short-lived' change
                        // stream cursors
        10 * 60 * 1e6,  // lifespan <= 10 minutes will probably capture not 'short-lived', but
                        // before the default cursor timeout
        20 * 60 * 1e6,  // lifespan <= 20 minutes will probably capture increased probability for
                        // cursor timeouts
        60 * 60 * 1e6,  // lifespan <= 1 hour will probably capture some hourly patterns
        24 * 60 * 60 * 1e6,     // lifetime <= 1 day will probably capture some daily patterns
        7 * 24 * 60 * 60 * 1e6  // lifetime <= 1 week will probably capture some weekly patterns
    });
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamCursorsLifespan,
        "Lifespan of closed change stream cursors in microseconds.",
        otel::metrics::MetricUnit::kMicroseconds,
        opts);
}

// Constructs the counter for currently open change stream cursors (idle or pinned).
inline otel::metrics::UpDownCounter<int64_t>& createCursorsOpenTotal() {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.total",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
        otel::metrics::MetricNames::kChangeStreamCursorsOpenTotal,
        "Current number of open change stream cursors.",
        otel::metrics::MetricUnit::kCursors,
        opts);
}


// Constructs the counter for the number of currently pinned (active) change stream cursors. This
// counter corresponds to the OTEL metric "serverStatus.metrics.changeStreams.cursor.open.pinned".
inline otel::metrics::UpDownCounter<int64_t>& createCursorsOpenPinned() {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.pinned",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
        otel::metrics::MetricNames::kChangeStreamCursorsOpenPinned,
        "Current number of open change stream cursors.",
        otel::metrics::MetricUnit::kCursors,
        opts);
}

inline otel::metrics::Counter<int64_t>& createUpdateLookupCounter(otel::metrics::MetricName name,
                                                                  std::string dottedPath,
                                                                  std::string description) {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = std::move(dottedPath),
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return otel::metrics::MetricsService::instance().createInt64Counter(
        name, std::move(description), otel::metrics::MetricUnit::kEvents, opts);
}

inline otel::metrics::Histogram<int64_t>& createUpdateLookupLatency(otel::metrics::MetricName name,
                                                                    std::string dottedPath) {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = std::move(dottedPath),
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };

    // Latency buckets span ~50us..1s, the expected range for a local or remote post-image fetch.
    opts.explicitBucketBoundaries = {{50,
                                      100,
                                      250,
                                      500,
                                      1000,
                                      2500,
                                      5000,
                                      10000,
                                      25000,
                                      50000,
                                      100000,
                                      250000,
                                      500000,
                                      1000000}};
    opts.serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts;
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        name,
        "Latency of change stream updateLookup single-document lookups in microseconds.",
        otel::metrics::MetricUnit::kMicroseconds,
        opts);
}

inline otel::metrics::Histogram<int64_t>& createCursorBatchSizeHistogram() {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.option.cursor.batchSize",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    opts.serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts;
    opts.explicitBucketBoundaries = std::vector<double>({1, 10, 100, 1000, 10000});
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize,
        "Batch size requested for change stream aggregate/getMore cursors.",
        otel::metrics::MetricUnit::kCount,
        opts);
};

// Histogram for the OTEL metric "serverStatus.metrics.changeStreams.option.cursor.batchSize".
// Tracks the 'batchSize' request field of change stream aggregate/getMore commands, on both
// mongod and mongos.
// Needs to be created and registered already at static-initialization time rather than lazily on
// first use, because the metrics tree is frozen shortly after startup.
inline otel::metrics::Histogram<int64_t>& kCursorBatchSizeHistogram =
    createCursorBatchSizeHistogram();

inline otel::metrics::Histogram<int64_t>& cursorBatchSizeHistogram() {
    return kCursorBatchSizeHistogram;
}

inline otel::metrics::Histogram<int64_t>& createCursorMaxAwaitTimeMSHistogram() {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.option.cursor.maxAwaitTimeMS",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    opts.serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts;
    opts.explicitBucketBoundaries = std::vector<double>(
        {100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000});
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS,
        "maxAwaitTimeMS requested for change stream getMore commands, in milliseconds.",
        otel::metrics::MetricUnit::kMilliseconds,
        opts);
};

// Histogram for the OTEL metric "serverStatus.metrics.changeStreams.option.cursor.maxAwaitTimeMS".
// Tracks the change stream cursor 'maxAwaitTimeMS' option, which getMore commands carry as
// 'maxTimeMS', on both mongod and mongos.
// Needs to be created and registered already at static-initialization time rather than lazily on
// first use, because the metrics tree is frozen shortly after startup.
inline otel::metrics::Histogram<int64_t>& kCursorMaxAwaitTimeMSHistogram =
    createCursorMaxAwaitTimeMSHistogram();

inline otel::metrics::Histogram<int64_t>& cursorMaxAwaitTimeMSHistogram() {
    return kCursorMaxAwaitTimeMSHistogram;
}

// Records the 'batchSize' of a change stream aggregate/getMore command, if the parsed request
// provides one (which may include an IDL-backfilled default, e.g. aggregate may backfill a default
// cursor.batchSize when the client omits it). Must only be called for change streams.
inline void recordCursorBatchSize(boost::optional<std::int64_t> batchSize) {
    if (batchSize) {
        cursorBatchSizeHistogram().record(*batchSize);
    }
}

// Records the change stream cursor 'maxAwaitTimeMS' option, which a getMore carries as its
// 'maxTimeMS'. Must only be called for change stream getMore commands: the 'maxTimeMS' of an
// aggregate is a different option and must not be recorded here.
inline void recordCursorMaxAwaitTimeMS(boost::optional<std::int64_t> maxTimeMS) {
    if (maxTimeMS) {
        cursorMaxAwaitTimeMSHistogram().record(*maxTimeMS);
    }
}

// TODO SERVER-130815: deduplicate metric initializers
inline otel::metrics::Counter<int64_t>& errorNonRetriableHistoryLost() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.error.nonRetriable.changeStreamHistoryLost",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamErrorNonRetriableHistoryLost,
            "Number of change stream errors: non-retriable ChangeStreamHistoryLost.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& errorNonRetriableFatalError() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.error.nonRetriable.changeStreamFatalError",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamErrorNonRetriableFatalError,
            "Number of change stream errors: non-retriable ChangeStreamFatalError.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& errorNonRetriableBsonObjectTooLarge() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.error.nonRetriable.bsonObjectTooLarge",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamErrorNonRetriableBsonObjectTooLarge,
            "Number of change stream errors: non-retriable BSONObjectTooLarge.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& errorNonRetriableOther() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.error.nonRetriable.other",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamErrorNonRetriableOther,
            "Number of change stream errors: non-retriable other.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& errorRetriableInterruptedDueToReplStateChange() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.error.retriable.interruptedDueToReplStateChange",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamErrorRetriableInterruptedDueToReplStateChange,
            "Number of change stream errors: retriable InterruptedDueToReplStateChange.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& errorRetriableOther() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.error.retriable.other",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamErrorRetriableOther,
            "Number of change stream errors: retriable other.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

// Returns true if 'code' represents a countable change stream error.
// Excludes:
//   - CloseChangeStream / ChangeStreamInvalidated: normal lifecycle transitions, not errors.
//   - MaxTimeMSExpired: routine awaitData getMore timeout surfaced by the ARM as a non-OK
//     Status; it is already tracked via _maxTimeMSExpired and should not pollute error counters.
inline bool shouldCountChangeStreamError(ErrorCodes::Error code) {
    return code != ErrorCodes::CloseChangeStream && code != ErrorCodes::ChangeStreamInvalidated &&
        code != ErrorCodes::MaxTimeMSExpired;
}

// Increments the appropriate change-stream error counter for the given error code.
// Must only be called when the cursor is a change stream (i.e. isChangeStreamQuery() == true).
inline void incrementChangeStreamErrorCounters(ErrorCodes::Error code) {
    switch (code) {
        case ErrorCodes::ChangeStreamHistoryLost:
            errorNonRetriableHistoryLost().add(1);
            return;
        case ErrorCodes::ChangeStreamFatalError:
            errorNonRetriableFatalError().add(1);
            return;
        case ErrorCodes::BSONObjectTooLarge:
            errorNonRetriableBsonObjectTooLarge().add(1);
            return;
        // InterruptedDueToReplStateChange is also in RetriableError; this named case must
        // precede the isA<RetriableError>() fallback in the default branch or it would be
        // miscounted as retriable.other.
        case ErrorCodes::InterruptedDueToReplStateChange:
            errorRetriableInterruptedDueToReplStateChange().add(1);
            return;
        default:
            // Catches any NonResumableChangeStreamError code not explicitly named above (currently
            // ShardRemovedError). Uses the generated category predicate so future additions to the
            // category are handled automatically rather than silently falling through to
            // nonRetriableOther.
            if (ErrorCodes::isA<ErrorCategory::NonResumableChangeStreamError>(code)) {
                errorNonRetriableOther().add(1);
            } else if (ErrorCodes::isA<ErrorCategory::RetriableError>(code)) {
                errorRetriableOther().add(1);
            } else {
                errorNonRetriableOther().add(1);
            }
    }
}

// Overload for DBException — delegates to the code-based overload above.
inline void incrementChangeStreamErrorCounters(const DBException& ex) {
    incrementChangeStreamErrorCounters(ex.code());
}

inline otel::metrics::Counter<int64_t>& cursorDocsReturned() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.cursor.docsReturned",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamCursorDocsReturned,
            "Total number of documents returned by change stream cursors.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& cursorBytesReturned() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.cursor.bytesReturned",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamCursorBytesReturned,
            "Total number of bytes returned by change stream cursors.",
            otel::metrics::MetricUnit::kBytes,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& cursorBatchesReturned() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.cursor.batchesReturned",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamCursorBatchesReturned,
            "Total number of batches returned by change stream cursors.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& cursorDocsExamined() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.cursor.docsExamined",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamCursorDocsExamined,
            "Total number of documents examined by change stream cursors.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& cursorBytesRead() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.cursor.bytesRead",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamCursorBytesRead,
            "Total number of bytes read by change stream cursors.",
            otel::metrics::MetricUnit::kBytes,
            opts);
    }();
    return counter;
}

// Public so the catalog_and_routing module's shard-targeter metrics can reuse the same
// serverStatus registration shape.
[[MONGO_MOD_PUBLIC]] inline otel::metrics::Counter<int64_t>& createShardTargetingCounter(
    otel::metrics::MetricName name, std::string dottedPath, std::string description) {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = std::move(dottedPath),
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return otel::metrics::MetricsService::instance().createInt64Counter(
        name, std::move(description), otel::metrics::MetricUnit::kEvents, opts);
}

inline otel::metrics::UpDownCounter<int64_t>& createShardTargetingDegradedGauge() {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.shardTargeting.degraded",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingDegraded,
        "Current number of v2 change streams whose topology-handler stage is in degraded mode.",
        otel::metrics::MetricUnit::kCount,
        opts);
}

// Public so the catalog_and_routing module's shard-targeter metrics can reuse the same
// serverStatus registration shape.
[[MONGO_MOD_PUBLIC]] inline otel::metrics::Histogram<int64_t>& createPlacementHistoryLatency(
    otel::metrics::MetricName name, std::string dottedPath) {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = std::move(dottedPath),
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };

    // Millisecond-scale buckets: a placement-history lookup is a config-server round trip, so
    // latencies land in the tens-to-thousands-of-milliseconds range.
    opts.explicitBucketBoundaries =
        std::vector<double>({1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000});
    opts.serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts;
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        name,
        "Latency of v2 change stream shard-targeting placement history lookups in milliseconds.",
        otel::metrics::MetricUnit::kMilliseconds,
        opts);
}

// --- End-of-transaction (EOT) change stream metrics ---
//
// All accessors use function-local statics; the eager-registration block in
// 'document_source_change_stream.cpp' touches them at static-init time so they are visible in
// serverStatus at zero. A successful rebuild must bump 'endOfTransactionRebuildsSucceeded()' and
// record its latency into 'endOfTransactionRebuildTimeMicros()' together; a failed rebuild records
// neither.

inline otel::metrics::Counter<int64_t>& endOfTransactionRebuildsSucceeded() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.rebuilds.succeeded",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionRebuildsSucceeded,
            "Number of on-demand end-of-transaction rebuilds that completed the resume at the "
            "consumer's position.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& endOfTransactionRebuildsFailed() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.rebuilds.failed",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionRebuildsFailed,
            "Number of end-of-transaction rebuilds that reached the rebuild cap and failed the "
            "stream.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Histogram<int64_t>& endOfTransactionRebuildTimeMicros() {
    static auto& histogram = []() -> otel::metrics::Histogram<int64_t>& {
        otel::metrics::HistogramOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.rebuildTimeMicros",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        // Successful rebuilds only: from catching the lookback kickback to the rebuilt stream
        // having resumed at the consumer's resume position. Spans a cheap kickback to a slow
        // oplog re-scan. Kept under the 10-bucket cap requested by MaaS.
        opts.serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts;
        opts.explicitBucketBoundaries = std::vector<double>({1e3, 1e4, 1e5, 1e6, 1e7, 1e8});
        return otel::metrics::MetricsService::instance().createInt64Histogram(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionRebuildTimeMicros,
            "Latency of successful end-of-transaction rebuilds in microseconds.",
            otel::metrics::MetricUnit::kMicroseconds,
            opts);
    }();
    return histogram;
}

inline otel::metrics::Counter<int64_t>& endOfTransactionSynthesisTransactionsEvicted() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.synthesis.transactions.evicted",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionSynthesisTransactionsEvicted,
            "Number of tracked transactions evicted because an expected participant's accounting "
            "record was never observed (whenAvailable disposition: event silently omitted).",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& endOfTransactionSynthesisPbrtBumps() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.synthesis.pbrtBumps",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionSynthesisPbrtBumps,
            "Number of explicit advances of the client-visible post-batch resume token to the "
            "emitted end-of-transaction token.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::UpDownCounter<int64_t>& endOfTransactionSynthesisTransactionsTracked() {
    static auto& counter = []() -> otel::metrics::UpDownCounter<int64_t>& {
        otel::metrics::UpDownCounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.synthesis.transactions.tracked",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionSynthesisTransactionsTracked,
            "Current number of transaction state entries held by router-side end-of-transaction "
            "aggregators, summed across all open change streams.",
            otel::metrics::MetricUnit::kCount,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& endOfTransactionCompletedWithRemovedShards() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.completedWithRemovedShards",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionCompletedWithRemovedShards,
            "Number of endOfTransaction events emitted best-effort after a participant shard was "
            "removed (ignore-removed-shards mode). A subset of "
            "changeStreams.event.endOfTransaction.transformed.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& endOfTransactionLookbackDistanceExceeded() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.lookbackDistanceExceeded",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionLookbackDistanceExceeded,
            "Number of lookbacks refused because the floor was further behind the resume point "
            "than the maximum lookback distance.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Counter<int64_t>& endOfTransactionRequiredMissingMetadata() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.requiredMissingMetadata",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::ShardServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionRequiredMissingMetadata,
            "Number of required change streams that failed because they encountered a transaction "
            "commit entry without the required endOfTransaction metadata.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

// Shard-server-only: downstream filters (resume-point swallowing, user stages) may still drop a
// transformed event.
inline otel::metrics::Counter<int64_t>& eventEndOfTransactionTransformed() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.event.endOfTransaction.transformed",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::ShardServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEventEndOfTransactionTransformed,
            "Number of endOfTransaction change events transformed on this node, including events "
            "later filtered out.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

// Router-only: emission is guaranteed once the event survives the user sub-pipeline.
inline otel::metrics::Counter<int64_t>& eventEndOfTransactionEmitted() {
    static auto& counter = []() -> otel::metrics::Counter<int64_t>& {
        otel::metrics::CounterOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.event.endOfTransaction.emitted",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::RouterServer},
        };
        return otel::metrics::MetricsService::instance().createInt64Counter(
            otel::metrics::MetricNames::kChangeStreamEventEndOfTransactionEmitted,
            "Number of endOfTransaction change events emitted to the consumer.",
            otel::metrics::MetricUnit::kEvents,
            opts);
    }();
    return counter;
}

inline otel::metrics::Gauge<int64_t>& endOfTransactionAllowEndOfTransactionGauge() {
    static auto& gauge = []() -> otel::metrics::Gauge<int64_t>& {
        otel::metrics::GaugeOptions opts{};
        opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
            .dottedPath = "changeStreams.endOfTransaction.allowEndOfTransaction",
            .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
        };
        return otel::metrics::MetricsService::instance().createInt64Gauge(
            otel::metrics::MetricNames::kChangeStreamEndOfTransactionAllowEndOfTransaction,
            "Whether the allowEndOfTransaction server parameter is currently enabled (1) or "
            "disabled (0).",
            otel::metrics::MetricUnit::kBoolean,
            opts);
    }();
    return gauge;
}

// Per-operationType transformed-event counters under changeStreams.event.<type>. Currently only
// 'endOfTransaction' (shard-server-only); later work extends this to regular event types.
// The literal matches DocumentSourceChangeStream::kEndOfTransactionOpType; keep in sync.
inline void recordChangeEventTransformed(std::string_view operationType) {
    if (operationType == "endOfTransaction") {
        eventEndOfTransactionTransformed().add(1);
    }
}

}  // namespace mongo::change_stream

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
#include <vector>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
namespace mongo::change_stream {

inline otel::metrics::CounterOptions kCursorsTotalOpenedOpts = [] {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.totalOpened",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();

// Constructs the counter for the OTEL metric
// "serverStatus.metrics.changeStreams.cursor.totalOpened".
inline otel::metrics::Counter<int64_t>& createCurorsTotalOpened() {
    return otel::metrics::MetricsService::instance().createInt64Counter(
        otel::metrics::MetricNames::kChangeStreamCursorsTotalOpened,
        "Total number of change stream cursors opened (on router or shard).",
        otel::metrics::MetricUnit::kCursors,
        kCursorsTotalOpenedOpts);
}

// Constructs the histogram for the OTEL metric
// "serverStatus.metrics.changeStreams.cursor.lifespan". The change stream lifespan histogram is
// updated after a change stream cursor is closed. A histogram provides accurate and thread-safe
// average for every bucket. This is achieved by locks, so there might be some overhead.
inline otel::metrics::HistogramOptions kLifespanOpts = [] {
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
    return opts;
}();

inline otel::metrics::Histogram<int64_t>& createCursorsLifespan() {
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamCursorsLifespan,
        "Lifespan of closed change stream cursors in microseconds.",
        otel::metrics::MetricUnit::kMicroseconds,
        kLifespanOpts);
}

inline otel::metrics::UpDownCounterOptions kCursorsOpenTotalOpts = [] {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.total",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();

// Constructs the counter for currently open change stream cursors (idle or pinned).
inline otel::metrics::UpDownCounter<int64_t>& createCursorsOpenTotal() {
    return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
        otel::metrics::MetricNames::kChangeStreamCursorsOpenTotal,
        "Current number of open change stream cursors.",
        otel::metrics::MetricUnit::kCursors,
        kCursorsOpenTotalOpts);
}


inline otel::metrics::UpDownCounterOptions kCursorsOpenPinnedOpts = [] {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.pinned",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();

// Constructs the counter for the number of currently pinned (active) change stream cursors. This
// counter corresponds to the OTEL metric "serverStatus.metrics.changeStreams.cursor.open.pinned".
inline otel::metrics::UpDownCounter<int64_t>& createCursorsOpenPinned() {
    return otel::metrics::MetricsService::instance().createInt64UpDownCounter(
        otel::metrics::MetricNames::kChangeStreamCursorsOpenPinned,
        "Current number of open change stream cursors.",
        otel::metrics::MetricUnit::kCursors,
        kCursorsOpenPinnedOpts);
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

inline otel::metrics::Histogram<int64_t>& createCursorMaxTimeMSHistogram() {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.option.cursor.maxTimeMS",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    opts.serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts;
    opts.explicitBucketBoundaries = std::vector<double>(
        {100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000});
    return otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxTimeMS,
        "maxTimeMS requested for change stream aggregate/getMore commands, in milliseconds.",
        otel::metrics::MetricUnit::kMilliseconds,
        opts);
};

// Histogram for the OTEL metric "serverStatus.metrics.changeStreams.option.cursor.maxTimeMS".
// Tracks the 'maxTimeMS' request field of change stream aggregate/getMore commands, on both
// mongod and mongos.
// Needs to be created and registered already at static-initialization time rather than lazily on
// first use, because the metrics tree is frozen shortly after startup.
inline otel::metrics::Histogram<int64_t>& kCursorMaxTimeMSHistogram =
    createCursorMaxTimeMSHistogram();

inline otel::metrics::Histogram<int64_t>& cursorMaxTimeMSHistogram() {
    return kCursorMaxTimeMSHistogram;
}

// Records cursor option usage metrics for a change stream aggregate/getMore command. Must only
// be called when the command/cursor is a change stream. Each option is recorded if the parsed
// request provides a value for it (which may include IDL-backfilled defaults, e.g. aggregate
// may backfill a default cursor.batchSize when the client omits it).
inline void recordCursorOptionMetrics(boost::optional<std::int64_t> batchSize,
                                      boost::optional<std::int64_t> maxTimeMS) {
    if (batchSize) {
        cursorBatchSizeHistogram().record(*batchSize);
    }
    if (maxTimeMS) {
        cursorMaxTimeMSHistogram().record(*maxTimeMS);
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

}  // namespace mongo::change_stream

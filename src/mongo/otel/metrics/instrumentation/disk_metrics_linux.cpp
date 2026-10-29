// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/instrumentation/disk_metrics.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/otel_metric_name_validation.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/procparser.h"

#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

using otel::metrics::Counter;
using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

constexpr std::string_view kDiskStatsPath = "/proc/diskstats"sv;
constexpr std::string_view kSysBlockPath = "/sys/block"sv;

struct DiskCounters {
    Counter<int64_t>* reads{nullptr};
    Counter<int64_t>* readSectors{nullptr};
    Counter<int64_t>* readTimeMs{nullptr};
    Counter<int64_t>* writes{nullptr};
    Counter<int64_t>* writeSectors{nullptr};
    Counter<int64_t>* writeTimeMs{nullptr};
    Counter<int64_t>* ioTimeMs{nullptr};
    Counter<int64_t>* ioQueuedMs{nullptr};
};

struct DiskMetricsState {
    std::unique_ptr<DiskMetrics> metrics;
    std::vector<std::string> diskNames;
    std::vector<std::string_view> diskViews;
    PeriodicJobAnchor job;
};

const auto getDiskMetricsState = ServiceContext::declareDecoration<DiskMetricsState>();

}  // namespace

class DiskMetrics::Impl {
public:
    explicit Impl(std::vector<std::string> disks) {
        _instruments.reserve(disks.size());

        // Skip any disk whose name produces an invalid metric name.
        // TODO (SERVER-131016): Include these.
        for (auto& disk : disks) {
            const std::string testName = fmt::format("systemMetrics.disks.{}.reads", disk);
            if (Status s = otel::metrics::validateOtelMetricName(testName); !s.isOK()) {
                LOGV2_DEBUG(13054301,
                            2,
                            "Skipping unsupported OTel disk metric name",
                            "disk"_attr = disk,
                            "error"_attr = s);
                continue;
            }

            const auto makeCounter = [&](std::string_view field,
                                         std::string desc,
                                         MetricUnit unit) -> Counter<int64_t>* {
                std::string fullName = fmt::format("systemMetrics.disks.{}.{}", disk, field);
                auto passkey = DiskMetrics::dyn_metric_passkey();
                return &MetricsService::instance().createInt64Counter(
                    DynamicMetricNameMaker::make(std::string_view{fullName}, passkey),
                    std::move(desc),
                    unit);
            };

            auto& instrument = _instruments.emplace_back();
            instrument.reads = makeCounter(
                "reads", "Number of read operations completed", MetricUnit::kOperations);
            instrument.readSectors =
                makeCounter("read_sectors", "Number of sectors read", MetricUnit::kCount);
            instrument.readTimeMs =
                makeCounter("read_time_ms", "Time spent reading", MetricUnit::kMilliseconds);
            instrument.writes = makeCounter(
                "writes", "Number of write operations completed", MetricUnit::kOperations);
            instrument.writeSectors =
                makeCounter("write_sectors", "Number of sectors written", MetricUnit::kCount);
            instrument.writeTimeMs =
                makeCounter("write_time_ms", "Time spent writing", MetricUnit::kMilliseconds);
            instrument.ioTimeMs = makeCounter(
                "io_time_ms", "Time disk was busy doing I/O", MetricUnit::kMilliseconds);
            instrument.ioQueuedMs = makeCounter("io_queued_ms",
                                                "Weighted time spent in the disk I/O queue",
                                                MetricUnit::kMilliseconds);
            _disks.push_back(std::move(disk));
        }
    }

    void update(BSONObj disksBson) {
        if (!_previousBson) {
            _previousBson = std::move(disksBson);
            return;
        }

        for (size_t i = 0; i < _disks.size(); ++i) {
            const BSONElement prev = (*_previousBson)[_disks[i]];
            const BSONElement curr = disksBson[_disks[i]];

            if (prev.type() != BSONType::object || curr.type() != BSONType::object) {
                continue;
            }

            addDeltas(_instruments[i], prev.Obj(), curr.Obj());
        }

        _previousBson = std::move(disksBson);
    }

private:
    void addDeltas(DiskCounters& instr, const BSONObj& prev, const BSONObj& curr) {
        const auto delta = [&](std::string_view field) {
            return std::max(0LL, curr[field].safeNumberLong() - prev[field].safeNumberLong());
        };

        instr.reads->add(delta("reads"));
        instr.readSectors->add(delta("read_sectors"));
        instr.readTimeMs->add(delta("read_time_ms"));
        instr.writes->add(delta("writes"));
        instr.writeSectors->add(delta("write_sectors"));
        instr.writeTimeMs->add(delta("write_time_ms"));
        instr.ioTimeMs->add(delta("io_time_ms"));
        instr.ioQueuedMs->add(delta("io_queued_ms"));
    }

    std::vector<std::string> _disks;
    std::vector<DiskCounters> _instruments;
    boost::optional<BSONObj> _previousBson;
};

DiskMetrics::DiskMetrics(std::vector<std::string> disks)
    : _impl(std::make_unique<Impl>(std::move(disks))) {}

DiskMetrics::~DiskMetrics() = default;

void DiskMetrics::update(BSONObj disksBson) {
    _impl->update(std::move(disksBson));
}

void installDiskOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getDiskMetricsState(svcCtx);

    state.diskNames = procparser::findPhysicalDisks(kSysBlockPath);
    if (state.diskNames.empty()) {
        return;
    }

    state.diskViews.reserve(state.diskNames.size());
    for (const auto& name : state.diskNames) {
        state.diskViews.push_back(name);
    }

    state.metrics = std::make_unique<DiskMetrics>(state.diskNames);

    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "DiskOtelMetrics",
        [&state](Client*) {
            BSONObjBuilder builder;
            Status s =
                procparser::parseProcDiskStatsFile(kDiskStatsPath, state.diskViews, &builder);
            if (s.isOK()) {
                state.metrics->update(builder.obj());
            } else {
                static logv2::SeveritySuppressor suppressor(
                    Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(3));

                if (auto sev = suppressor(); shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, sev)) {
                    LOGV2_DEBUG(
                        12006910, sev.toInt(), "Failed to collect disk stats", "error"_attr = s);
                }
            }
        },
        Seconds(1),
        false /*isKillableByStepdown*/});

    state.job.start();
}

}  // namespace mongo

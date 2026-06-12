/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/instrumentation/disk_metrics.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/procparser.h"

#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {

using otel::metrics::Counter;
using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

constexpr StringData kDiskStatsPath = "/proc/diskstats"_sd;
constexpr StringData kSysBlockPath = "/sys/block"_sd;

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
    std::vector<StringData> diskViews;
    PeriodicJobAnchor job;
};

const auto getDiskMetricsState = ServiceContext::declareDecoration<DiskMetricsState>();

}  // namespace

class DiskMetrics::Impl {
public:
    explicit Impl(std::vector<std::string> disks) : _disks(std::move(disks)) {
        _instruments.resize(_disks.size());

        for (size_t i = 0; i < _disks.size(); ++i) {
            const std::string& disk = _disks[i];
            const auto makeCounter =
                [&](StringData field, std::string desc, MetricUnit unit) -> Counter<int64_t>* {
                std::string fullName = fmt::format("systemMetrics.disks.{}.{}", disk, field);
                auto passkey = DiskMetrics::dyn_metric_passkey();
                return &MetricsService::instance().createInt64Counter(
                    DynamicMetricNameMaker::make(StringData{fullName}, passkey),
                    std::move(desc),
                    unit);
            };

            _instruments[i].reads = makeCounter(
                "reads", "Number of read operations completed", MetricUnit::kOperations);
            _instruments[i].readSectors =
                makeCounter("read_sectors", "Number of sectors read", MetricUnit::kCount);
            _instruments[i].readTimeMs =
                makeCounter("read_time_ms", "Time spent reading", MetricUnit::kMilliseconds);
            _instruments[i].writes = makeCounter(
                "writes", "Number of write operations completed", MetricUnit::kOperations);
            _instruments[i].writeSectors =
                makeCounter("write_sectors", "Number of sectors written", MetricUnit::kCount);
            _instruments[i].writeTimeMs =
                makeCounter("write_time_ms", "Time spent writing", MetricUnit::kMilliseconds);
            _instruments[i].ioTimeMs = makeCounter(
                "io_time_ms", "Time disk was busy doing I/O", MetricUnit::kMilliseconds);
            _instruments[i].ioQueuedMs = makeCounter("io_queued_ms",
                                                     "Weighted time spent in the disk I/O queue",
                                                     MetricUnit::kMilliseconds);
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
        const auto delta = [&](StringData field) {
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

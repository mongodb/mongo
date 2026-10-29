// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/instrumentation/system_mount_metrics.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/otel_metric_name_validation.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/procparser.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::Gauge;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

struct MountGauges {
    Gauge<int64_t>* capacity{nullptr};
    Gauge<int64_t>* available{nullptr};
    Gauge<int64_t>* free{nullptr};
};

struct MountOtelMetricsState {
    std::unique_ptr<SystemMountMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getMountOtelMetricsState = ServiceContext::declareDecoration<MountOtelMetricsState>();

constexpr std::string_view kMountInfoPath = "/proc/self/mountinfo"sv;

// Sanitize a mount path for use as a metric name segment:
//   "/"          -> "root"
//   "/data"      -> "data"
//   "/boot/efi"  -> "boot.efi"
std::string sanitizeMountpoint(std::string_view path) {
    if (path == "/") {
        return "root";
    }

    // Strip leading slash and replace interior slashes with dots.
    std::string result(path.substr(1));
    std::replace(result.begin(), result.end(), '/', '.');
    return result;
}

std::vector<std::string> enumerateMountpoints() {
    BSONObjBuilder probe;

    const Status status = procparser::parseProcSelfMountStatsFile(kMountInfoPath, &probe);

    if (!status.isOK()) {
        LOGV2_WARNING(12006900,
                      "Failed to enumerate mountpoints for OTel system mount metrics",
                      "error"_attr = status);
        return {};
    }

    const BSONObj probeObj = probe.obj();
    std::vector<std::string> mountpoints;
    std::ranges::transform(probeObj, std::back_inserter(mountpoints), [](const BSONElement& entry) {
        return std::string(entry.fieldName());
    });

    std::ranges::sort(mountpoints);
    mountpoints.erase(std::ranges::unique(mountpoints).begin(), mountpoints.end());

    return mountpoints;
}

std::unique_ptr<SystemMountMetrics> makeMetrics() {
    auto mountpoints = enumerateMountpoints();
    if (mountpoints.empty()) {
        return nullptr;
    }

    try {
        return std::make_unique<SystemMountMetrics>(std::move(mountpoints));
    } catch (const DBException& ex) {
        LOGV2_ERROR(
            12006901, "Failed to register OTel system mount metrics", "error"_attr = ex.toStatus());
        return nullptr;
    }
}

}  // namespace

class SystemMountMetrics::Impl {
public:
    explicit Impl(std::vector<std::string> mountpoints) {
        _instruments.reserve(mountpoints.size());
        _mountpoints.reserve(mountpoints.size());

        // Skip any mountpoint whose name produces an invalid metric name.
        // TODO (SERVER-131016): Include these.
        for (auto& mountpoint : mountpoints) {
            auto sanitized = sanitizeMountpoint(mountpoint);
            const std::string testName = fmt::format("systemMetrics.mounts.{}.capacity", sanitized);
            if (Status s = otel::metrics::validateOtelMetricName(testName); !s.isOK()) {
                LOGV2_DEBUG(13054300,
                            2,
                            "Skipping unsupported OTel system mount metric name",
                            "mountpoint"_attr = mountpoint,
                            "error"_attr = s);
                continue;
            }
            _mountpoints.push_back(std::move(mountpoint));

            const auto makeGauge =
                [&](std::string_view field, std::string desc, MetricUnit unit) -> Gauge<int64_t>* {
                std::string fullName = fmt::format("systemMetrics.mounts.{}.{}", sanitized, field);
                auto passkey = SystemMountMetrics::dyn_metric_passkey();
                return &MetricsService::instance().createInt64Gauge(
                    DynamicMetricNameMaker::make(std::string_view{fullName}, passkey),
                    std::move(desc),
                    unit);
            };

            auto& instrument = _instruments.emplace_back();
            instrument.capacity =
                makeGauge("capacity", "Total filesystem capacity in bytes", MetricUnit::kBytes);
            instrument.available =
                makeGauge("available", "Filesystem space available in bytes", MetricUnit::kBytes);
            instrument.free =
                makeGauge("free", "Total free filesystem space in bytes", MetricUnit::kBytes);
        }
    }

    void update(const BSONObj& mountsBson) {
        for (size_t i = 0; i < _mountpoints.size(); ++i) {
            const BSONElement entry = mountsBson[_mountpoints[i]];
            if (entry.type() != BSONType::object) {
                continue;
            }

            const BSONObj stats = entry.Obj();
            _instruments[i].capacity->set(stats["capacity"].safeNumberLong());
            _instruments[i].available->set(stats["available"].safeNumberLong());
            _instruments[i].free->set(stats["free"].safeNumberLong());
        }
    }

private:
    std::vector<std::string> _mountpoints;
    std::vector<MountGauges> _instruments;
};

SystemMountMetrics::SystemMountMetrics(std::vector<std::string> mountpoints)
    : _impl(std::make_unique<Impl>(std::move(mountpoints))) {}

SystemMountMetrics::~SystemMountMetrics() = default;

void SystemMountMetrics::update(const BSONObj& mountsBson) {
    _impl->update(mountsBson);
}

void installSystemMountOtelMetrics(ServiceContext* svcCtx) {
    auto metrics = makeMetrics();
    if (!metrics) {
        return;
    }

    auto& state = getMountOtelMetricsState(svcCtx);
    state.metrics = std::move(metrics);
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "SystemMountOtelMetrics",
        [&state](Client*) {
            BSONObjBuilder builder;
            Status s = procparser::parseProcSelfMountStatsFile(kMountInfoPath, &builder);
            if (s.isOK()) {
                state.metrics->update(builder.obj());
            } else {
                static logv2::SeveritySuppressor suppressor(
                    Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(3));

                if (auto sev = suppressor(); shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, sev)) {
                    LOGV2_DEBUG(
                        12006902, sev.toInt(), "Failed to collect mount stats", "error"_attr = s);
                }
            }
        },
        Seconds(1),
        false /*isKillableByStepdown*/});

    state.job.start();
}

}  // namespace mongo

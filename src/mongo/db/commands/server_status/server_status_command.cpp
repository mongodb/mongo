// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"

#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
constexpr auto kTimingSection = "timing"sv;

class CmdServerStatus : public BasicCommand {
public:
    CmdServerStatus() : BasicCommand("serverStatus"), _started(Date_t::now()) {}

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool allowsAfterClusterTime(const BSONObj& cmdObj) const final {
        return false;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    std::string help() const final {
        return "returns lots of administrative server statistics. Optionally set {..., all: 1} to "
               "retrieve all server status sections.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::serverStatus)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        const auto service = opCtx->getServiceContext();
        const auto clock = service->getFastClockSource();
        const auto runStart = clock->now();
        BSONObjBuilder timeBuilder(256);
        Date_t phaseStart = runStart;
        auto recordPhase = [&](std::string_view name) {
            const Date_t t = clock->now();
            timeBuilder.appendNumber(fmt::format("after {}", name),
                                     durationCount<Milliseconds>(t - phaseStart));
            phaseStart = t;
        };

        ScopedAdmissionPriority<ExecutionAdmissionContext> admissionPriority(
            opCtx, AdmissionContext::Priority::kExempt);

        // --- basic fields that are global

        result.append("host", prettyHostName(opCtx->getClient()->getLocalPort()));
        result.append("version", VersionInfoInterface::instance().version());
        result.append("process", serverGlobalParams.binaryName);
        result.append("pid", ProcessId::getCurrent().asLongLong());
        result.append("uptime", (double)(time(nullptr) - serverGlobalParams.started));
        auto uptime = clock->now() - _started;
        result.append("uptimeMillis", durationCount<Milliseconds>(uptime));
        result.append("uptimeEstimate", durationCount<Seconds>(uptime));
        result.appendDate("localTime", Date_t::now());

        // Milliseconds spent building the fixed header fields above (not cumulative since start).
        recordPhase("basic"sv);

        // Individual section 'includeByDefault()' settings will be bypassed if the caller specified
        // {all: 1}.
        const auto& allElem = cmdObj["all"];
        bool includeAllSections = stdx::to_underlying(allElem.type()) ? allElem.trueValue() : false;

        const auto& noneElem = cmdObj["none"];
        bool excludeAllSections =
            stdx::to_underlying(noneElem.type()) ? noneElem.trueValue() : false;

        if (MONGO_unlikely(includeAllSections && excludeAllSections)) {
            // {all: 1} and {none: 1} cannot both be specified.
            uasserted(ErrorCodes::InvalidOptions, "Cannot provide both 'all' and 'none' options");
        }


        // --- all sections
        auto registry = ServerStatusSectionRegistry::instance();
        for (auto i = registry->begin(); i != registry->end(); ++i) {
            auto& section = i->second;

            if (!section->checkAuthForOperation(opCtx).isOK()) {
                continue;
            }

            bool include = !excludeAllSections && section->includeByDefault();
            const auto& elem = cmdObj[section->getSectionName()];
            if (stdx::to_underlying(elem.type())) {
                include = elem.trueValue();
            }

            if (!include && !includeAllSections) {
                continue;
            }

            if (!section->relevantTo(opCtx->getService()->role())) {
                continue;
            }

            try {
                section->appendSection(opCtx, elem, &result);
            } catch (...) {
                auto status = exceptionToStatus();
                if (!ErrorCodes::isA<ErrorCategory::ShutdownError>(status.code())) {
                    LOGV2_INFO(9761501,
                               "Section threw an error",
                               "error"_attr = status,
                               "section"_attr = section->getSectionName());
                }
                throw;
            }
            recordPhase(section->getSectionName());
        }

        // --- counters
        auto metricsEl = cmdObj["metrics"sv];
        if ((!excludeAllSections && metricsEl.eoo()) || metricsEl.trueValue()) {
            // Always gather the role-agnostic metrics. If `opCtx` has a role,
            // additionally merge that role's associated metrics.
            std::vector<const MetricTree*> metricTrees;
            auto& treeSet = globalMetricTreeSet();
            metricTrees.push_back(&treeSet[ClusterRole::None]);
            if (auto svc = opCtx->getService())
                metricTrees.push_back(&treeSet[svc->role()]);
            BSONObj excludePaths;
            if (metricsEl.type() == BSONType::object)
                excludePaths = BSON("metrics" << metricsEl.embeddedObject());
            appendMergedTrees(metricTrees, result, excludePaths);
            recordPhase("metrics"sv);
        }

        // --- some hard coded global things hard to pull out

        const Date_t endTime = clock->now();
        timeBuilder.appendNumber("other", durationCount<Milliseconds>(endTime - phaseStart));
        const auto runElapsed = endTime - runStart;
        timeBuilder.appendNumber("at end", durationCount<Milliseconds>(runElapsed));
        if (runElapsed > Milliseconds(1000)) {
            BSONObj t = timeBuilder.obj();
            LOGV2(20499, "serverStatus was very slow", "timeStats"_attr = t);

            bool include_timing = true;
            const auto& elem = cmdObj[kTimingSection];
            if (!elem.eoo()) {
                include_timing = elem.trueValue();
            }

            if (include_timing) {
                result.append(kTimingSection, t);
            }
        }

        return true;
    }

private:
    const Date_t _started;
};

MONGO_REGISTER_COMMAND(CmdServerStatus).forRouter().forShard();

}  // namespace

auto globalOpCounterServerStatusSection =
    *ServerStatusSectionBuilder<OpCounterServerStatusSection>("opcounters")
         .bind(&globalOpCounters());

namespace {

// some universal sections

class ExtraInfo : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder bb;

        bb.append("note", "fields vary by platform");
        ProcessInfo p;
        p.getExtraInfo(bb);

        return bb.obj();
    }
};
// Register one instance of the section shared by both roles; this contains process-wide info.
auto extraInfo = *ServerStatusSectionBuilder<ExtraInfo>("extra_info").forShard().forRouter();

class Asserts : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder asserts;
        asserts.append("regular", assertionCount.regular.loadRelaxed());
        asserts.append("warning", assertionCount.warning.loadRelaxed());
        asserts.append("msg", assertionCount.msg.loadRelaxed());
        asserts.append("user", assertionCount.user.loadRelaxed());
        asserts.append("tripwire", assertionCount.tripwire.loadRelaxed());
        asserts.append("rollovers", assertionCount.rollovers.loadRelaxed());
        return asserts.obj();
    }
};
auto asserts = *ServerStatusSectionBuilder<Asserts>("asserts");

struct MemBaseMetricPolicy {
    void appendTo(BSONObjBuilder& bob, std::string_view leafName) const {
        BSONObjBuilder b{bob.subobjStart(leafName)};
        b.append("bits", static_cast<int>(sizeof(void*) * CHAR_BIT));

        ProcessInfo p;
        if (p.supported()) {
            b.appendNumber("resident", p.getResidentSize());
            b.appendNumber("virtual", p.getVirtualMemorySize());
            b.appendBool("supported", true);
        } else {
            b.append("note", "not all mem info support on this platform");
            b.appendBool("supported", false);
        }

        auto secAlloc = getSecureAllocatorStats();
        b.appendNumber("secureAllocByteCount", static_cast<long long>(secAlloc.bytes));
        b.appendNumber("secureAllocBytesInPages", static_cast<long long>(secAlloc.pages));
    }
};
auto& memBase = *CustomMetricBuilder<MemBaseMetricPolicy>{".mem"};

class HttpClientServerStatus : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const final {
        return false;
    }

    BSONObj generateSection(OperationContext*, const BSONElement& configElement) const final {
        return HttpClient::getServerStatus();
    }
};
// Register one instance of the section shared by both roles; this contains process-wide info.
auto httpClientServerStatus =
    *ServerStatusSectionBuilder<HttpClientServerStatus>("http_client").forShard().forRouter();

}  // namespace

}  // namespace mongo

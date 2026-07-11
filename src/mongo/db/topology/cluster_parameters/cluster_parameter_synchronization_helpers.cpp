// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_parameter_synchronization_helpers.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/audit.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/functional.h"

#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo::cluster_parameters {
using namespace std::literals::string_view_literals;
namespace {

constexpr auto kIdField = "_id"sv;
constexpr auto kCPTField = "clusterParameterTime"sv;
constexpr auto kOplog = "oplog"sv;

void clearParameter(OperationContext* opCtx,
                    ServerParameter* sp,
                    const boost::optional<TenantId>& tenantId) {
    if (sp->getClusterParameterTime(tenantId) == LogicalTime::kUninitialized) {
        // Nothing to clear.
        return;
    }

    // Callback handlers of the ServerParameters take operation context and are allowed to acquire
    // Shared and ExclusiveLock (ResourceMutex). These mutex acquisitions are not allowed to throw
    // and not allowed to do any blocking work either. Placing this ULG here covers the first
    // condition.
    UninterruptibleLockGuard ulg(opCtx);  // NOLINT (ResourceMutex acquisition)

    BSONObjBuilder oldValueBob;
    sp->append(opCtx, &oldValueBob, sp->name(), tenantId);

    uassertStatusOK(sp->reset(tenantId));

    BSONObjBuilder newValueBob;
    sp->append(opCtx, &newValueBob, sp->name(), tenantId);

    audit::logUpdateCachedClusterParameter(
        opCtx->getClient(), oldValueBob.obj(), newValueBob.obj(), tenantId);
}

void doLoadAllTenantParametersFromCollection(
    OperationContext* opCtx,
    const Collection& coll,
    std::string_view mode,
    unique_function<
        void(OperationContext*, const BSONObj&, std::string_view, const boost::optional<TenantId>&)>
        onEntry) try {
    invariant(coll.ns() == NamespaceString::makeClusterParametersNSS(coll.ns().tenantId()));

    // If the RecoveryUnit already had an open snapshot, keep the snapshot open. Otherwise abandon
    // the snapshot when exiting the function.
    ScopeGuard scopeGuard([&] { shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot(); });
    if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
        scopeGuard.dismiss();
    }

    std::vector<Status> failures;

    auto cursor = coll.getCursor(opCtx);
    for (auto doc = cursor->next(); doc; doc = cursor->next()) {
        try {
            auto data = doc.get().data.toBson();
            validateParameter(data, coll.ns().tenantId());
            onEntry(opCtx, data, mode, coll.ns().tenantId());
        } catch (const DBException& ex) {
            failures.push_back(ex.toStatus());
        }
    }

    if (!failures.empty()) {
        StringBuilder msg;
        for (const auto& failure : failures) {
            msg << failure.toString() << ", ";
        }
        msg.reset(msg.len() - 2);
        uasserted(ErrorCodes::OperationFailed, msg.str());
    }
} catch (const DBException& ex) {
    uassertStatusOK(ex.toStatus().withContext(
        str::stream() << "Failed " << mode << " cluster server parameters from disk"));
}

}  // namespace

void validateParameter(BSONObj doc, const boost::optional<TenantId>& tenantId) {
    auto nameElem = doc[kIdField];
    uassert(ErrorCodes::OperationFailed,
            "Validate with invalid parameter name",
            nameElem.type() == BSONType::string);
    auto name = nameElem.valueStringData();
    auto* sp = ServerParameterSet::getClusterParameterSet()->getIfExists(name);
    uassert(ErrorCodes::OperationFailed, "Validate on unknown cluster parameter", sp);
    uassertStatusOK(sp->validate(doc, tenantId));
}

void updateParameter(OperationContext* opCtx,
                     BSONObj doc,
                     std::string_view mode,
                     const boost::optional<TenantId>& tenantId) {
    auto nameElem = doc[kIdField];
    if (nameElem.type() != BSONType::string) {
        LOGV2_DEBUG(6226301,
                    1,
                    "Update with invalid cluster server parameter name",
                    "mode"_attr = mode,
                    "tenantId"_attr = tenantId,
                    "_id"_attr = nameElem);
        return;
    }

    auto name = nameElem.valueStringData();
    auto* sp = ServerParameterSet::getClusterParameterSet()->getIfExists(name);
    if (!sp) {
        LOGV2_DEBUG(6226300,
                    3,
                    "Update to unknown cluster server parameter",
                    "mode"_attr = mode,
                    "tenantId"_attr = tenantId,
                    "name"_attr = name);
        return;
    }

    auto cptElem = doc[kCPTField];
    if ((cptElem.type() != BSONType::timestamp)) {
        LOGV2_DEBUG(6226302,
                    1,
                    "Update to cluster server parameter has invalid clusterParameterTime",
                    "mode"_attr = mode,
                    "tenantId"_attr = tenantId,
                    "name"_attr = name,
                    "clusterParameterTime"_attr = cptElem);
        return;
    }

    uassertStatusOK(sp->validate(doc, tenantId));

    // Callback handlers of the ServerParameters take operation context and are allowed to acquire
    // Shared and ExclusiveLock (ResourceMutex). These mutex acquisitions are not allowed to throw
    // and not allowed to do any blocking work either. Placing this ULG here covers the first
    // condition.
    UninterruptibleLockGuard ulg(opCtx);  // NOLINT (ResourceMutex acquisition)

    BSONObjBuilder oldValueBob;
    sp->append(opCtx, &oldValueBob, std::string{name}, tenantId);
    audit::logUpdateCachedClusterParameter(opCtx->getClient(), oldValueBob.obj(), doc, tenantId);

    uassertStatusOK(sp->set(doc, tenantId));
}

void clearParameter(OperationContext* opCtx,
                    std::string_view id,
                    const boost::optional<TenantId>& tenantId) {
    auto* sp = ServerParameterSet::getClusterParameterSet()->getIfExists(id);
    if (!sp) {
        LOGV2_DEBUG(6226303,
                    5,
                    "oplog event deletion of unknown cluster server parameter",
                    "name"_attr = id,
                    "tenantId"_attr = tenantId);
        return;
    }

    clearParameter(opCtx, sp, tenantId);
}

void clearAllTenantParameters(OperationContext* opCtx, const boost::optional<TenantId>& tenantId) {
    const auto& params = ServerParameterSet::getClusterParameterSet()->getMap();
    for (const auto& it : params) {
        clearParameter(opCtx, it.second.get(), tenantId);
    }
}

void initializeAllTenantParametersFromCollection(OperationContext* opCtx, const Collection& coll) {
    doLoadAllTenantParametersFromCollection(opCtx,
                                            coll,
                                            "initializing"sv,
                                            [&](OperationContext* opCtx,
                                                const BSONObj& doc,
                                                std::string_view mode,
                                                const boost::optional<TenantId>& tenantId) {
                                                updateParameter(opCtx, doc, mode, tenantId);
                                            });
}

void resynchronizeAllTenantParametersFromCollection(OperationContext* opCtx,
                                                    const Collection& coll) {
    const auto& allParams = ServerParameterSet::getClusterParameterSet()->getMap();
    std::set<std::string> unsetSettings;
    for (const auto& it : allParams) {
        unsetSettings.insert(it.second->name());
    }

    doLoadAllTenantParametersFromCollection(opCtx,
                                            coll,
                                            "resynchronizing"sv,
                                            [&](OperationContext* opCtx,
                                                const BSONObj& doc,
                                                std::string_view mode,
                                                const boost::optional<TenantId>& tenantId) {
                                                unsetSettings.erase(doc[kIdField].str());
                                                updateParameter(opCtx, doc, mode, tenantId);
                                            });

    // For all known settings which were not present in this resync,
    // explicitly clear any value which may be present in-memory.
    for (const auto& setting : unsetSettings) {
        clearParameter(opCtx, setting, coll.ns().tenantId());
    }
}

}  // namespace mongo::cluster_parameters

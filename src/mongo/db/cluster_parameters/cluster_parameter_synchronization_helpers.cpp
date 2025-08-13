/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/cluster_parameters/cluster_parameter_synchronization_helpers.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/audit.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/server_parameter.h"
#include "mongo/logv2/log.h"
#include "mongo/util/functional.h"

#include <set>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo::cluster_parameters {
namespace {

constexpr auto kIdField = "_id"_sd;
constexpr auto kCPTField = "clusterParameterTime"_sd;
constexpr auto kOplog = "oplog"_sd;

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
    StringData mode,
    unique_function<
        void(OperationContext*, const BSONObj&, StringData, const boost::optional<TenantId>&)>
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
                     StringData mode,
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
    if ((cptElem.type() != BSONType::date) && (cptElem.type() != BSONType::timestamp)) {
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
                    StringData id,
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
                                            "initializing"_sd,
                                            [&](OperationContext* opCtx,
                                                const BSONObj& doc,
                                                StringData mode,
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
                                            "resynchronizing"_sd,
                                            [&](OperationContext* opCtx,
                                                const BSONObj& doc,
                                                StringData mode,
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

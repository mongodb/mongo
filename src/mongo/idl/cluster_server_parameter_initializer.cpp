/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/idl/cluster_server_parameter_initializer.h"

#include "mongo/base/string_data.h"
#include "mongo/db/audit.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
const auto getInstance = ServiceContext::declareDecoration<ClusterServerParameterInitializer>();
const ReplicaSetAwareServiceRegistry::Registerer<ClusterServerParameterInitializer> _registerer(
    "ClusterServerParameterInitializerRegistry");

constexpr auto kIdField = "_id"_sd;
constexpr auto kCPTField = "clusterParameterTime"_sd;
constexpr auto kOplog = "oplog"_sd;

}  // namespace

ClusterServerParameterInitializer* ClusterServerParameterInitializer::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ClusterServerParameterInitializer* ClusterServerParameterInitializer::get(
    ServiceContext* serviceContext) {
    return &getInstance(serviceContext);
}

void ClusterServerParameterInitializer::updateParameter(OperationContext* opCtx,
                                                        BSONObj doc,
                                                        StringData mode) {
    auto nameElem = doc[kIdField];
    if (nameElem.type() != String) {
        LOGV2_DEBUG(6226301,
                    1,
                    "Update with invalid cluster server parameter name",
                    "mode"_attr = mode,
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
                    "name"_attr = name);
        return;
    }

    auto cptElem = doc[kCPTField];
    if ((cptElem.type() != mongo::Date) && (cptElem.type() != bsonTimestamp)) {
        LOGV2_DEBUG(6226302,
                    1,
                    "Update to cluster server parameter has invalid clusterParameterTime",
                    "mode"_attr = mode,
                    "name"_attr = name,
                    "clusterParameterTime"_attr = cptElem);
        return;
    }

    BSONObjBuilder oldValueBob;
    sp->append(opCtx, &oldValueBob, name.toString(), boost::none);
    audit::logUpdateCachedClusterParameter(opCtx->getClient(), oldValueBob.obj(), doc);

    uassertStatusOK(sp->set(doc, boost::none));
}

void ClusterServerParameterInitializer::clearParameter(OperationContext* opCtx,
                                                       ServerParameter* sp) {
    if (sp->getClusterParameterTime(boost::none) == LogicalTime::kUninitialized) {
        // Nothing to clear.
        return;
    }

    BSONObjBuilder oldValueBob;
    sp->append(opCtx, &oldValueBob, sp->name(), boost::none);

    uassertStatusOK(sp->reset(boost::none));

    BSONObjBuilder newValueBob;
    sp->append(opCtx, &newValueBob, sp->name(), boost::none);

    audit::logUpdateCachedClusterParameter(
        opCtx->getClient(), oldValueBob.obj(), newValueBob.obj());
}

void ClusterServerParameterInitializer::clearParameter(OperationContext* opCtx, StringData id) {
    auto* sp = ServerParameterSet::getClusterParameterSet()->getIfExists(id);
    if (!sp) {
        LOGV2_DEBUG(6226303,
                    5,
                    "oplog event deletion of unknown cluster server parameter",
                    "name"_attr = id);
        return;
    }

    clearParameter(opCtx, sp);
}

void ClusterServerParameterInitializer::clearAllParameters(OperationContext* opCtx) {
    const auto& params = ServerParameterSet::getClusterParameterSet()->getMap();
    for (const auto& it : params) {
        clearParameter(opCtx, it.second);
    }
}

void ClusterServerParameterInitializer::initializeAllParametersFromDisk(OperationContext* opCtx) {
    doLoadAllParametersFromDisk(
        opCtx, "initializing"_sd, [this](OperationContext* opCtx, BSONObj doc, StringData mode) {
            updateParameter(opCtx, doc, mode);
        });
}

void ClusterServerParameterInitializer::resynchronizeAllParametersFromDisk(
    OperationContext* opCtx) {
    const auto& allParams = ServerParameterSet::getClusterParameterSet()->getMap();
    std::set<std::string> unsetSettings;
    for (const auto& it : allParams) {
        unsetSettings.insert(it.second->name());
    }

    doLoadAllParametersFromDisk(
        opCtx,
        "resynchronizing"_sd,
        [this, &unsetSettings](OperationContext* opCtx, BSONObj doc, StringData mode) {
            unsetSettings.erase(doc[kIdField].str());
            updateParameter(opCtx, doc, mode);
        });

    // For all known settings which were not present in this resync,
    // explicitly clear any value which may be present in-memory.
    for (const auto& setting : unsetSettings) {
        clearParameter(opCtx, setting);
    }
}

void ClusterServerParameterInitializer::onInitialDataAvailable(OperationContext* opCtx,
                                                               bool isMajorityDataAvailable) {
    LOGV2_INFO(6608200, "Initializing cluster server parameters from disk");
    initializeAllParametersFromDisk(opCtx);
}

}  // namespace mongo

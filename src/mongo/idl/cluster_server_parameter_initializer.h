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

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replica_set_aware_service.h"

namespace mongo {

/**
 * An interface that provides methods to manipulate in-memory cluster server parameter values in
 * response to on-disk changes, specifically in a replica set context.
 */
class ClusterServerParameterInitializer
    : public ReplicaSetAwareService<ClusterServerParameterInitializer> {
    ClusterServerParameterInitializer(const ClusterServerParameterInitializer&) = delete;
    ClusterServerParameterInitializer& operator=(const ClusterServerParameterInitializer&) = delete;

public:
    ClusterServerParameterInitializer() = default;
    ~ClusterServerParameterInitializer() = default;

    static ClusterServerParameterInitializer* get(OperationContext* opCtx);
    static ClusterServerParameterInitializer* get(ServiceContext* serviceContext);

    void updateParameter(OperationContext* opCtx,
                         BSONObj doc,
                         StringData mode,
                         const boost::optional<TenantId>& tenantId);
    void clearParameter(OperationContext* opCtx,
                        ServerParameter* sp,
                        const boost::optional<TenantId>& tenantId);
    void clearParameter(OperationContext* opCtx,
                        StringData id,
                        const boost::optional<TenantId>& tenantId);
    void clearAllTenantParameters(OperationContext* opCtx,
                                  const boost::optional<TenantId>& tenantId);

    /**
     * Used to initialize in-memory cluster parameter state based on the on-disk contents after
     * startup recovery or initial sync is complete.
     */
    void initializeAllTenantParametersFromDisk(OperationContext* opCtx,
                                               const boost::optional<TenantId>& tenantId);

    /**
     * Used on rollback and rename with drop.
     * Updates settings which are present and clears settings which are not.
     */
    void resynchronizeAllTenantParametersFromDisk(OperationContext* opCtx,
                                                  const boost::optional<TenantId>& tenantId);

    // Virtual methods coming from the ReplicaSetAwareService
    void onStartup(OperationContext* opCtx) override final {}

    /**
     * Called after startup recovery or initial sync is complete.
     */
    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final;
    void onShutdown() override final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override final {}
    void onStepUpComplete(OperationContext* opCtx, long long term) override final {}
    void onStepDown() override final {}
    void onBecomeArbiter() override final {}

private:
    template <typename OnEntry>
    void doLoadAllTenantParametersFromDisk(OperationContext* opCtx,
                                           StringData mode,
                                           OnEntry onEntry,
                                           const boost::optional<TenantId>& tenantId) try {
        std::vector<Status> failures;

        DBDirectClient client(opCtx);

        FindCommandRequest findRequest{NamespaceString::makeClusterParametersNSS(tenantId)};
        client.find(std::move(findRequest), [&](BSONObj doc) {
            try {
                onEntry(opCtx, doc, mode, tenantId);
            } catch (const DBException& ex) {
                failures.push_back(ex.toStatus());
            }
        });
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
};

}  // namespace mongo

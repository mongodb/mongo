/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <set>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/config.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

const std::set<std::string> kUnsupportedTenantIds{"", "admin", "local", "config"};

}  // namespace

namespace tenant_migration_util {

inline Status validateDatabasePrefix(const std::string& tenantId) {
    const bool isPrefixSupported =
        kUnsupportedTenantIds.find(tenantId) == kUnsupportedTenantIds.end();

    return isPrefixSupported
        ? Status::OK()
        : Status(ErrorCodes::BadValue,
                 str::stream() << "cannot migrate databases for tenant \'" << tenantId << "'");
}

inline Status validateTimestampNotNull(const Timestamp& ts) {
    return (!ts.isNull())
        ? Status::OK()
        : Status(ErrorCodes::BadValue, str::stream() << "Timestamp can't be null");
}

inline Status validateConnectionString(const StringData& donorOrRecipientConnectionString) {
    const auto donorOrRecipientUri =
        uassertStatusOK(MongoURI::parse(donorOrRecipientConnectionString.toString()));
    const auto donorOrRecipientServers = donorOrRecipientUri.getServers();

    // Sanity check to make sure that the given donor or recipient connection string corresponds
    // to a replica set connection string with at least one host.
    try {
        const auto donorOrRecipientRsConnectionString = ConnectionString::forReplicaSet(
            donorOrRecipientUri.getSetName(),
            std::vector<HostAndPort>(donorOrRecipientServers.begin(),
                                     donorOrRecipientServers.end()));
    } catch (const ExceptionFor<ErrorCodes::FailedToParse>& ex) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Donor and recipient must be a replica set with at least one host: "
                          << ex.toStatus());
    }

    // Sanity check to make sure that donor and recipient do not share any hosts.
    const auto servers = repl::ReplicationCoordinator::get(cc().getServiceContext())
                             ->getConfig()
                             .getConnectionString()
                             .getServers();

    for (auto&& server : servers) {
        bool foundMatch = std::any_of(
            donorOrRecipientServers.begin(),
            donorOrRecipientServers.end(),
            [&](const HostAndPort& donorOrRecipient) { return server == donorOrRecipient; });
        if (foundMatch) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Donor and recipient hosts must be different.");
        }
    }
    return Status::OK();
}

inline Status validateCertificatePEMPayload(const StringData& payload) {
#ifndef MONGO_CONFIG_SSL
    return {ErrorCodes::InternalError,
            "Could not validate certificate field as SSL is not supported"};
#else
    auto swBlob =
        ssl_util::findPEMBlob(payload, "CERTIFICATE"_sd, 0 /* position */, false /* allowEmpty */);
    return swBlob.getStatus().withContext("Invalid certificate field");
#endif
}

inline Status validatePrivateKeyPEMPayload(const StringData& payload) {
#ifndef MONGO_CONFIG_SSL
    return {ErrorCodes::InternalError,
            "Could not validate certificate field as SSL is not supported"};
#else
    auto swBlob =
        ssl_util::findPEMBlob(payload, "PRIVATE KEY"_sd, 0 /* position */, false /* allowEmpty */);
    return swBlob.getStatus().withContext("Invalid private key field");
#endif
}

/*
 * Creates an ExternalKeysCollectionDocument representing an admin.system.external_validation_keys
 * document from the given the admin.system.keys document BSONObj.
 */
ExternalKeysCollectionDocument makeExternalClusterTimeKeyDoc(ServiceContext* serviceContext,
                                                             std::string rsName,
                                                             BSONObj keyDoc);

/*
 * For each given ExternalKeysCollectionDocument, inserts it if there is not an existing document in
 * admin.system.external_validation_keys for it with the same keyId and replicaSetName. Otherwise,
 * updates the ttlExpiresAt of the existing document if it is less than the new ttlExpiresAt.
 * Refreshes the logical validator's cache before returning.
 */
void storeExternalClusterTimeKeyDocsAndRefreshCache(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::vector<ExternalKeysCollectionDocument> keyDocs,
    const CancelationToken& token);

/**
 * Creates a view on the oplog that allows a tenant migration recipient to fetch retryable writes
 * from a tenant migration donor.
 */
void createRetryableWritesView(OperationContext* opCtx, Database* db);

}  // namespace tenant_migration_util

}  // namespace mongo

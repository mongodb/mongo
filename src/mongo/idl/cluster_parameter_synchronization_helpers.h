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

#pragma once

#include "mongo/db/db_raii.h"

namespace mongo {

namespace cluster_parameters {

void updateParameter(BSONObj doc, StringData mode);

void clearParameter(ServerParameter* sp);

void clearParameter(StringData id);

void clearAllParameters();

/**
 * Used to initialize in-memory cluster parameter state based on the on-disk contents after startup
 * recovery or initial sync is complete.
 */
void initializeAllParametersFromDisk(OperationContext* opCtx);

/**
 * Used on rollback. Updates settings which are present and clears settings which are not.
 */
void resynchronizeAllParametersFromDisk(OperationContext* opCtx);

template <typename OnEntry>
void doLoadAllParametersFromDisk(OperationContext* opCtx, StringData mode, OnEntry onEntry) try {

    // If the RecoveryUnit already had an open snapshot, keep the snapshot open. Otherwise
    // abandon the snapshot when exiting the function.
    ScopeGuard scopeGuard([&] { opCtx->recoveryUnit()->abandonSnapshot(); });
    if (opCtx->recoveryUnit()->isActive()) {
        scopeGuard.dismiss();
    }

    AutoGetCollectionForRead coll(opCtx, NamespaceString::kClusterParametersNamespace);
    if (!coll) {
        return;
    }

    std::vector<Status> failures;

    auto cursor = coll->getCursor(opCtx);
    for (auto doc = cursor->next(); doc; doc = cursor->next()) {
        try {
            onEntry(opCtx, doc.get().data.toBson(), mode);
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

/**
 * Used after an importCollection commits. Will update the in-memory cluster parameter state if the
 * given namespace is a cluster parameters namespace.
 */
void maybeUpdateClusterParametersPostImportCollectionCommit(OperationContext* opCtx,
                                                            const NamespaceString& nss);

}  // namespace cluster_parameters

}  // namespace mongo

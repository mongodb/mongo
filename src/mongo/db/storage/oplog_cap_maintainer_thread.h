/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <string>

#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/background.h"

namespace mongo {

void startOplogCapMaintainerThread(ServiceContext* serviceContext,
                                   bool isReplSet,
                                   bool shouldSkipOplogSampling);

void stopOplogCapMaintainerThread(ServiceContext* serviceContext, const Status& reason);

/**
 * Responsible for deleting oplog truncate markers once their max capacity has been reached.
 */
class OplogCapMaintainerThread : public BackgroundJob {
public:
    OplogCapMaintainerThread() : BackgroundJob(false /* deleteSelf */) {}

    static OplogCapMaintainerThread* get(ServiceContext* serviceCtx);
    static OplogCapMaintainerThread* get(OperationContext* opCtx);
    static void set(ServiceContext* serviceCtx,
                    std::unique_ptr<OplogCapMaintainerThread> oplogCapMaintainerThread);

    std::string name() const override {
        return _name;
    }

    void run() override;

    /**
     * Waits until the maintainer thread finishes. Must not be called concurrently with start().
     */
    void shutdown(const Status& reason);

private:
    /**
     * Returns true iff there was an oplog to delete from.
     */
    bool _deleteExcessDocuments(OperationContext* opCtx);

    // Serializes setting/resetting _uniqueCtx and marking _uniqueCtx killed.
    mutable stdx::mutex _opCtxMutex;

    // Saves a reference to the cap maintainer thread's operation context.
    boost::optional<ServiceContext::UniqueOperationContext> _uniqueCtx;

    mutable stdx::mutex _stateMutex;
    bool _shuttingDown = false;
    Status _shutdownReason = Status::OK();

    std::string _name = std::string("OplogCapMaintainerThread-") +
        toStringForLogging(NamespaceString::kRsOplogNamespace);
};

}  // namespace mongo

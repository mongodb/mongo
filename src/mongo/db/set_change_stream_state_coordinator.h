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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/set_change_stream_state_coordinator_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * A 'PrimaryOnlyService::Instance' that runs in the replica sets and orchestrates the change stream
 * requests in the serverless.
 *
 * At any time only one request is accepted, any subsequent request will be rejected by this
 * instance type.
 */
class SetChangeStreamStateCoordinator : public DefaultPrimaryOnlyServiceInstance {
public:
    explicit SetChangeStreamStateCoordinator(const BSONObj& stateDoc);

    StringData getInstanceName() final;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _removeStateDocument(OperationContext* opCtx) final;

    // The state document of the 'PrimaryOnlyService'.
    SetChangeStreamStateCoordinatorDocument _stateDoc;

    // Stores the state document durably in the namespace 'config.change_stream_coordinator'.
    PrimaryOnlyServiceStateStore<SetChangeStreamStateCoordinatorDocument> _stateDocStore;
};

/**
 * A 'PrimaryOnlyService' that creates and gets an instance of the
 * 'SetChangeStreamStateCoordinator'.
 */
class SetChangeStreamStateCoordinatorService final : public repl::PrimaryOnlyService {
public:
    explicit SetChangeStreamStateCoordinatorService(ServiceContext* serviceContext)
        : repl::PrimaryOnlyService(serviceContext) {}

    ~SetChangeStreamStateCoordinatorService() override = default;

    StringData getServiceName() const final;

    NamespaceString getStateDocumentsNS() const final;

    ThreadPool::Limits getThreadPoolLimits() const final;

    std::shared_ptr<SetChangeStreamStateCoordinator> getOrCreateInstance(OperationContext* opCtx,
                                                                         BSONObj coorDoc);

    static SetChangeStreamStateCoordinatorService* getService(OperationContext* opCtx);

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) final {};

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) final;
};
}  // namespace mongo

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

#include "mongo/db/internal_session_pool.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/config/set_user_write_block_mode_coordinator_document_gen.h"

namespace mongo {

ConfigsvrCoordinatorMetadata extractConfigsvrCoordinatorMetadata(const BSONObj& stateDoc);

/**
 * ConfigsvrCoordinators are POS instances that run on the configsvr and represent cluster
 * operations that are driven by the configsvr. ConfigsvrCoordinator implements common framework for
 * such operations. Concrete operations extend ConfigsvrCoordinator and implement their specific
 * bussiness logic on '_runImpl'
 */
class ConfigsvrCoordinator : public repl::PrimaryOnlyService::TypedInstance<ConfigsvrCoordinator> {
public:
    explicit ConfigsvrCoordinator(const BSONObj& stateDoc);

    ~ConfigsvrCoordinator();

    SharedSemiFuture<void> getCompletionFuture() {
        return _completionPromise.getFuture();
    }

    virtual bool hasSameOptions(const BSONObj&) const = 0;

protected:
    const ConfigsvrCoordinatorId _coordId;

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override final;

    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept = 0;

    void interrupt(Status status) noexcept override final;

    void _removeStateDocument(OperationContext* opCtx);

    Mutex _mutex = MONGO_MAKE_LATCH("ConfigsvrCoordinator::_mutex");
    SharedPromise<void> _completionPromise;
};

}  // namespace mongo

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

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"

namespace mongo {

/**
 * A helper to CRUD the state document for the 'PrimaryOnlyService' using the provided namespace
 * 'stateDocumentNs'.
 */
template <typename StateDocumentType>
class PrimaryOnlyServiceStateStore {
public:
    explicit PrimaryOnlyServiceStateStore(const NamespaceString& stateDocumentNs)
        : _stateDocumentNs{stateDocumentNs} {}

    /**
     * Stores the state document 'addDoc' on the provided namespace.
     */
    void add(
        OperationContext* opCtx,
        const StateDocumentType& addDoc,
        const WriteConcernOptions& writeConcern = WriteConcerns::kMajorityWriteConcernNoTimeout) {
        PersistentTaskStore<StateDocumentType> store(_stateDocumentNs);
        store.add(opCtx, addDoc, writeConcern);
    }

    /**
     * Removes the state document matching the criteria stated in the 'removeDoc'.
     */
    void remove(
        OperationContext* opCtx,
        const BSONObj& removeDoc,
        const WriteConcernOptions& writeConcern = WriteConcerns::kMajorityWriteConcernNoTimeout) {
        PersistentTaskStore<StateDocumentType> store(_stateDocumentNs);
        store.remove(opCtx, removeDoc, writeConcern);
    }

    /**
     * Updates the state document 'updateDoc' using the provided filter criteria 'filter'.
     */
    void update(
        OperationContext* opCtx,
        const BSONObj& filter,
        const BSONObj& updateDoc,
        const WriteConcernOptions& writeConcern = WriteConcerns::kMajorityWriteConcernNoTimeout) {
        PersistentTaskStore<StateDocumentType> store(_stateDocumentNs);
        store.update(opCtx, filter, updateDoc, writeConcern);
    }

    /**
     * Gets the count of the documents matching the 'filter' criteria.
     */
    size_t count(OperationContext* opCtx, const BSONObj& filter = BSONObj{}) {
        PersistentTaskStore<StateDocumentType> store(_stateDocumentNs);
        return store.count(opCtx, filter);
    }

private:
    // Namespace where the state document will be stored.
    const NamespaceString _stateDocumentNs;
};

/**
 * A helper that provides a default implementation for the set of methods to create the
 * 'PrimaryOnlyService::Instance'.
 */
class DefaultPrimaryOnlyServiceInstance
    : public repl::PrimaryOnlyService::TypedInstance<DefaultPrimaryOnlyServiceInstance> {
public:
    ~DefaultPrimaryOnlyServiceInstance();

    /**
     * The name of the 'PrimaryOnlyService::Instance'.
     */
    virtual StringData getInstanceName() = 0;

    /**
     * Interrupts the current running 'DefaultPrimaryOnlyServiceInstance' instance.
     */
    void interrupt(Status status) noexcept override;

    /**
     * Returns the completion state of the running instance.
     */
    SharedSemiFuture<void> getCompletionFuture();

private:
    /**
     * Invoked by the 'PrimaryOnlyService' to run the 'DefaultPrimaryOnlyServiceInstance' instance
     * type.
     */
    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    /**
     * The derived class must override this method to write the custom logic. This method is invoked
     * within the context of the 'run' method.
     */
    virtual ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept = 0;

    /**
     * Removes the state document after the 'DefaultPrimaryOnlyServiceInstance' has been completed.
     * It is the responsibility of the derived class to store the state document.
     */
    virtual void _removeStateDocument(OperationContext* opCtx) = 0;

    // Guards the access of the '_completionPromise'.
    Mutex _mutex = MONGO_MAKE_LATCH("DefaultPrimaryOnlyServiceInstance::_mutex");

    // Tracks the completion state of the instance.
    SharedPromise<void> _completionPromise;
};

}  // namespace mongo

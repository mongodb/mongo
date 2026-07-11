// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {

/**
 * A helper to CRUD the state document for the 'PrimaryOnlyService' using the provided namespace
 * 'stateDocumentNs'.
 */
template <typename StateDocumentType>
class [[MONGO_MOD_OPEN]] PrimaryOnlyServiceStateStore {
public:
    explicit PrimaryOnlyServiceStateStore(const NamespaceString& stateDocumentNs)
        : _stateDocumentNs{stateDocumentNs} {}

    /**
     * Stores the state document 'addDoc' on the provided namespace.
     */
    void add(OperationContext* opCtx,
             const StateDocumentType& addDoc,
             const WriteConcernOptions& writeConcern = defaultMajorityWriteConcern()) {
        PersistentTaskStore<StateDocumentType> store(_stateDocumentNs);
        store.add(opCtx, addDoc, writeConcern);
    }

    /**
     * Removes the state document matching the criteria stated in the 'removeDoc'.
     */
    void remove(OperationContext* opCtx,
                const BSONObj& removeDoc,
                const WriteConcernOptions& writeConcern = defaultMajorityWriteConcern()) {
        PersistentTaskStore<StateDocumentType> store(_stateDocumentNs);
        store.remove(opCtx, removeDoc, writeConcern);
    }

    /**
     * Updates the state document 'updateDoc' using the provided filter criteria 'filter'.
     */
    void update(OperationContext* opCtx,
                const BSONObj& filter,
                const BSONObj& updateDoc,
                const WriteConcernOptions& writeConcern = defaultMajorityWriteConcern()) {
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
class [[MONGO_MOD_PRIVATE]] DefaultPrimaryOnlyServiceInstance
    : public repl::PrimaryOnlyService::TypedInstance<DefaultPrimaryOnlyServiceInstance> {
public:
    ~DefaultPrimaryOnlyServiceInstance() override;

    /**
     * The name of the 'PrimaryOnlyService::Instance'.
     */
    virtual std::string_view getInstanceName() = 0;

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
    std::mutex _mutex;

    // Tracks the completion state of the instance.
    SharedPromise<void> _completionPromise;
};

}  // namespace mongo

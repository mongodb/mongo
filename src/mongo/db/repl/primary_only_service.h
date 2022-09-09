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

#include <boost/optional.hpp>
#include <memory>
#include <unordered_set>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/string_map.h"

namespace mongo {

class OperationContext;
class ServiceContext;

namespace repl {

extern FailPoint PrimaryOnlyServiceHangBeforeRebuildingInstances;
extern FailPoint PrimaryOnlyServiceFailRebuildingInstances;
extern FailPoint PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic;

/**
 * A PrimaryOnlyService is a group of tasks (represented in memory as Instances) that should only
 * be run when the node is primary, and should continue running until completion even across
 * replica set failovers, resuming on the new primary after any successful election. Each service
 * will have a dedicated collection where state documents are stored containing the state of any
 * running instances, which are used to recreate the running instances after failover.
 */
class PrimaryOnlyService {
public:
    /**
     * Client decoration used by Clients that are a part of a PrimaryOnlyService.
     */
    struct PrimaryOnlyServiceClientState {
        PrimaryOnlyService* primaryOnlyService = nullptr;
        bool allowOpCtxWhenServiceRebuilding = false;
    };

    /**
     * Every instance must have an ID that is unique among instances of that service. The
     * InstanceID will be the _id of the state document corresponding to that instance.
     */
    using InstanceID = BSONObj;

    /**
     * A PrimaryOnlyService::Instance represents one running PrimaryOnlyService task.  These
     * correspond 1-1 to state documents persisted in the service's state document collection.
     * Instances objects live for no longer than the length of a single primary's term, however the
     * tasks they conceptually represent can live on across terms. Instance objects are released on
     * stepDown, and recreated on stepUp if they have not yet finished their work (and therefore
     * have a corresponding document in the service's state document collection). After being
     * released from the PrimaryOnlyService object on stepdown, instance objects may continue to
     * exist until the subsequent stepUp. During stepUp, however, we join() any remaining tasks from
     * the previous term, guaranteeing that there will never be 2 Instance objects representing the
     * same conceptual task coexisting. Instance objects are released from the PrimaryOnlyService
     * object when their corresponding state documents are removed.
     * NOTE: PrimaryOnlyService
     * implementations shouldn't have their Instance subclass extended this Instance class directly,
     * instead they should extend TypedInstance, defined below.
     */
    class Instance {
    public:
        virtual ~Instance() = default;

        /**
         * This is the main function that PrimaryOnlyService implementations will need to implement,
         * and is where the bulk of the work those services perform is scheduled. All work run for
         * this Instance *must* be scheduled on 'executor'. Instances are responsible for inserting,
         * updating, and deleting their state documents as needed.
         *
         * IMPORTANT NOTES:
         * 1. Once the state document for this Instance is deleted, all shared_ptr
         * references to this Instance that are managed by the PrimaryOnlyService machinery are
         * removed, so all work running on behalf of this Instance must extend the Instance's
         * lifetime by getting a shared_ptr via 'shared_from_this' or else the Instance may be
         * destroyed out from under them.
         *
         * 2. On stepdown/shutdown of a PrimaryOnlyService, the input cancellation token will be
         * marked canceled.
         */
        virtual SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                     const CancellationToken& token) noexcept = 0;

        /**
         * This is the function that is called when this running Instance needs to be interrupted.
         * It should unblock any work managed by this Instance by, for example, emplacing the given
         * error into any unresolved promises that the Instance manages.
         */
        virtual void interrupt(Status status) = 0;

        /**
         * Returns a BSONObj containing information about the state of this running Instance, to be
         * reported in currentOp() output, or boost::none if this Instance should not show up in
         * currentOp, based on the given 'connMode' and 'sessionMode' that currentOp() is running
         * with.
         */
        virtual boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept = 0;

        /**
         * Validate the found instance matches options in the state document of a new instance.
         * Called in PrimaryOnlyService::getOrCreateInstance to check found instances for conflict.
         * Throws an exception if state document conflicts with found instance.
         */
        virtual void checkIfOptionsConflict(const BSONObj& stateDoc) const = 0;
    };

    /**
     * Extends Instance with a template specifier that subclasses should specialize on
     * themselves. This allows providing a function to lookup an Instance object that returns the
     * proper derived Instance type.
     */
    template <class InstanceType>
    class TypedInstance : public Instance, public std::enable_shared_from_this<InstanceType> {
    public:
        TypedInstance() = default;
        virtual ~TypedInstance() = default;

        /**
         * Same functionality as PrimaryOnlyService::lookupInstance, but returns a pointer of
         * the proper derived class for the Instance.
         */
        static boost::optional<std::shared_ptr<InstanceType>> lookup(OperationContext* opCtx,
                                                                     PrimaryOnlyService* service,
                                                                     const InstanceID& id) {
            auto instance = service->lookupInstance(opCtx, id);
            if (!instance) {
                return boost::none;
            }

            return checked_pointer_cast<InstanceType>(instance.get());
        }

        /**
         * Same functionality as PrimaryOnlyService::getOrCreateInstance, but returns a pointer of
         * the proper derived class for the Instance.
         */
        static std::shared_ptr<InstanceType> getOrCreate(OperationContext* opCtx,
                                                         PrimaryOnlyService* service,
                                                         BSONObj initialState,
                                                         bool checkOptions = true) {
            auto [instance, _] =
                service->getOrCreateInstance(opCtx, std::move(initialState), checkOptions);
            return checked_pointer_cast<InstanceType>(instance);
        }
    };

    explicit PrimaryOnlyService(ServiceContext* serviceContext);
    virtual ~PrimaryOnlyService() = default;

    /**
     * Returns the name of this Primary Only Service.
     */
    virtual StringData getServiceName() const = 0;

    /**
     * Returns the collection where state documents corresponding to instances of this service are
     * persisted.
     */
    virtual NamespaceString getStateDocumentsNS() const = 0;

    /**
     * Returns the limits that should be imposed on the size of the underlying thread pool used for
     * running Instances of this PrimaryOnlyService.
     */
    virtual ThreadPool::Limits getThreadPoolLimits() const = 0;

    /**
     * Constructs and starts up _executor.
     */
    void startup(OperationContext* opCtx);

    /**
     * Releases all running Instances, then shuts down and joins _executor, ensuring that
     * there are no remaining tasks running.
     */
    void shutdown();

    /**
     * Called on transition to primary. Resumes any running Instances of this service
     * based on their persisted state documents (after waiting for the first write of the new term
     * to be majority committed). Also joins() any outstanding jobs from the previous term, thereby
     * ensuring that two Instance objects with the same InstanceID cannot coexist.
     */
    void onStepUp(const OpTime& stepUpOpTime);

    /**
     * Called on stepDown. Releases all running Instances of this service from management by this
     * PrimaryOnlyService object. The Instances will have their OperationContexts interrupted
     * independently. Instance objects may continue to exist in memory in a detached state until the
     * next stepUp. Also shuts down _executor, forcing all outstanding jobs to complete.
     */
    void onStepDown();

    /**
     * Releases the shared_ptr for the given InstanceID (if present) from management by this
     * service. This is called by the OpObserver when a state document in this service's state
     * document collection is deleted, and is the main way that instances get removed from
     * _activeInstances and deleted.
     * If 'status' is not OK, it is passed as argument to the interrupt() method on the instance.
     */
    void releaseInstance(const InstanceID& id, Status status);

    /**
     * Releases all Instances from _activeInstances. Called by the OpObserver if this service's
     * state document collection is dropped. If 'status' is not OK, it is passed as argument to the
     * interrupt() method on each instance.
     */
    void releaseAllInstances(Status status);

    /**
     * Adds information of this service to the result 'BSONObjBuilder', containing the number of
     * active instances and the state of this service.
     */
    void reportForServerStatus(BSONObjBuilder* result) noexcept;

    /**
     * Adds information about the Instances belonging to this service to 'ops', to show up in
     * currentOp(). 'connMode' and 'sessionMode' are arguments provided to currentOp, and can be
     * used to make decisions about whether or not various Instances should actually show up in the
     * currentOp() output for this invocation of currentOp().
     */
    void reportInstanceInfoForCurrentOp(MongoProcessInterface::CurrentOpConnectionsMode connMode,
                                        MongoProcessInterface::CurrentOpSessionsMode sessionMode,
                                        std::vector<BSONObj>* ops) noexcept;

    /**
     * Registers that this OperationContext is being used by a thread running on behalf of this
     * PrimaryOnlyService.  Ensures that this OperationContext will be interrupted during stepDown.
     * If this service is not currently running, pro-actively interrupts the opCtx, unless
     * 'allowOpCtxWhileRebuilding' is true and the current _state is kRebuilding.
     */
    void registerOpCtx(OperationContext* opCtx, bool allowOpCtxWhileRebuilding);

    /**
     * Unregisters a previously registered OperationContext. Indicates that this OpCtx is done
     * performing work (and most likely is about to be deleted) and thus doesn't need to be
     * interrupted at stepDown.
     */
    void unregisterOpCtx(OperationContext* opCtx);

protected:
    /**
     * Allows OpCtxs created on PrimaryOnlyService threads to remain uninterrupted, even if the
     * service they are associated with aren't in state kRunning, so long as the state is
     * kRebuilding instead. Used during the stepUp process to allow the database read or write
     * required to rebuild a service and get it running in the first place. Does not prevent other
     * forms of OpCtx interruption, such as from stepDown or calls to killOp.
     */
    class AllowOpCtxWhenServiceRebuildingBlock {
    public:
        explicit AllowOpCtxWhenServiceRebuildingBlock(Client* client);
        ~AllowOpCtxWhenServiceRebuildingBlock();

    private:
        Client* _client;
        PrimaryOnlyServiceClientState* _clientState;
    };

    /**
     * Validate the instance to be created with initialState does not conflict with any existing
     * ones. The implementation should throw ConflictingOperationInProgress if there is a conflict.
     */
    virtual void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const Instance*>& existingInstances) = 0;

    /**
     * Constructs a new Instance object with the given initial state.
     */
    virtual std::shared_ptr<Instance> constructInstance(BSONObj initialState) = 0;

    /**
     * Given an InstanceId returns the corresponding running Instance object, or boost::none if
     * there is none. If the service is in State::kRebuilding, will wait (interruptibly on the
     * opCtx) for the rebuild to complete.
     */
    boost::optional<std::shared_ptr<Instance>> lookupInstance(OperationContext* opCtx,
                                                              const InstanceID& id);

    /**
     * Extracts an InstanceID from the _id field of the given 'initialState' object. If an Instance
     * with the extracted InstanceID already exists in _activeInstances, returns true and the
     * instance itself.  If not, constructs a new Instance (by calling constructInstance()),
     * registers it in _activeInstances, and returns it with the boolean set to false. It is illegal
     * to call this more than once with 'initialState' documents that have the same _id but are
     * otherwise not completely identical.
     *
     * Returns a pair with an Instance and a boolean, the boolean indicates if the Instance have
     * been created in this invocation (true) or already existed (false).
     *
     * Throws NotWritablePrimary if the node is not currently primary.
     */
    std::pair<std::shared_ptr<Instance>, bool> getOrCreateInstance(OperationContext* opCtx,
                                                                   BSONObj initialState,
                                                                   bool checkOptions = true);

    /**
     * Since, scoped task executor shuts down on stepdown, we might need to run some instance work,
     * like cleanup, even while the node is not primary. So, use the parent executor in that case.
     */
    std::shared_ptr<executor::TaskExecutor> getInstanceCleanupExecutor() const;

    /**
     * Returns shared pointers to all Instance objects that belong to this service.
     */
    std::vector<std::shared_ptr<Instance>> getAllInstances(OperationContext* opCtx);

private:
    enum class State {
        kRunning,
        kPaused,
        kRebuilding,
        kRebuildFailed,
        kShutdown,
    };

    /**
     * Represents a PrimaryOnlyService::Instance that has already been scheduled to be run.
     */
    class ActiveInstance {
    public:
        ActiveInstance(std::shared_ptr<Instance> instance,
                       CancellationSource source,
                       SemiFuture<void> runCompleteFuture)
            : _instance(std::move(instance)),
              _runCompleteFuture(std::move(runCompleteFuture)),
              _source(std::move(source)) {
            invariant(_instance);
        }

        ActiveInstance(const ActiveInstance&) = delete;
        ActiveInstance& operator=(const ActiveInstance&) = delete;

        ActiveInstance(ActiveInstance&&) = delete;
        ActiveInstance& operator=(ActiveInstance&&) = delete;

        /**
         * Blocking call that returns once the instance has finished running.
         */
        void waitForCompletion() const {
            _runCompleteFuture.wait();
        }

        std::shared_ptr<Instance> getInstance() const {
            return _instance;
        }

        void interrupt(Status s) {
            _source.cancel();
            _instance->interrupt(std::move(s));
        }

    private:
        const std::shared_ptr<Instance> _instance;

        // A future that will be resolved when the passed in Instance has finished running.
        const SemiFuture<void> _runCompleteFuture;

        // Each instance of a PrimaryOnlyService will own a CancellationSource for memory management
        // purposes. Any memory associated with an instance's CancellationSource will be cleaned up
        // upon the destruction of an instance. It must be instantiated from a token from the
        // CancellationSource of the PrimaryOnlyService class in order to attain a hierarchical
        // ownership pattern that allows for cancellation token clean up if the PrimaryOnlyService
        // is shutdown/stepdown.
        CancellationSource _source;
    };

    /*
     * This method is called once the _executor is initialized. This can be called only once
     * in the lifetime of the POS object instance.
     */
    void _setHasExecutor(WithLock);
    /*
     * Returns true if the _executor is initialized.
     */
    bool _getHasExecutor() const;

    /**
     * Called as part of onSetUp before rebuilding instances. This function should do any additional
     * work required to rebuild the service on stepup, for example, creating a TTL index for the
     * state machine collection.
     */
    virtual ExecutorFuture<void> _rebuildService(
        std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
        return ExecutorFuture<void>(**executor, Status::OK());
    };

    /**
     * Called at the end of the service stepdown procedure.
     * In order to not block the stepdown procedure, no blocking work must be done in this
     * function.
     */
    virtual void _afterStepDown() {}

    /**
     * Called as part of onStepUp.  Queries the state document collection for this
     * PrimaryOnlyService, constructs Instance objects for each document found, and schedules work
     * to run all the newly recreated Instances.
     */
    void _rebuildInstances(long long term) noexcept;

    /**
     * Schedules work to call the provided instance's 'run' method and inserts the new instance into
     * the _activeInstances map as a ActiveInstance.
     */
    std::shared_ptr<PrimaryOnlyService::Instance> _insertNewInstance(
        WithLock, std::shared_ptr<Instance> instance, InstanceID instanceID);

    /**
     * Interrupts all running instances.
     */
    void _interruptInstances(WithLock, Status);

    /**
     * Returns a string representation of the current state.
     */
    StringData _getStateString(WithLock) const;

    /**
     *  Blocks until `_state` is not equal to `kRebuilding`. May release the mutex, but always
     * acquires it before returning.
     */
    void _waitForStateNotRebuilding(OperationContext* opCtx, BasicLockableAdapter m);

    /**
     * Updates `_state` with `newState` and notifies waiters on `_stateChangeCV`.
     */
    void _setState(State newState, WithLock);

    ServiceContext* const _serviceContext;

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex.
    // (W)  Synchronization required only for writes.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("PrimaryOnlyService::_mutex");

    // Condvar to receive notifications when _state changes.
    stdx::condition_variable _stateChangeCV;  // (S)

    // A ScopedTaskExecutor that is used to perform all work run on behalf of an Instance.
    // This ScopedTaskExecutor wraps _executor and is created at stepUp and destroyed at
    // stepDown so that all outstanding tasks get interrupted.
    std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;  // (M)

    // TODO SERVER-52901: Remove _hasExecutor.
    // Note: This method has to be accessed only via _setHasExecutor() and _getHasExecutor()
    // methods. This is present to make PrimaryOnlyService::_executor to have (W) synchronization
    // rule instead of (M).
    AtomicWord<bool> _hasExecutor{false};  //(S)

    // TODO SERVER-52901: Make the synchronization rule as (R).
    // The concrete TaskExecutor backing _scopedExecutor. While _scopedExecutor is created and
    // destroyed with each stepUp/stepDown, _executor persists for the lifetime of the process. We
    // want _executor to survive failover to prevent us from having to reallocate lots of
    // thread and connection resources on every stepUp. Service instances should never have access
    // to _executor directly, they should only ever use _scopedExecutor so that they get the
    // guarantee that all outstanding tasks are interrupted at stepDown.
    std::shared_ptr<executor::TaskExecutor> _executor;  // (W)

    State _state = State::kPaused;  // (M)

    // If reloading the state documents from disk fails, this Status gets set to a non-ok value and
    // calls to lookup() or getOrCreate() will throw this status until the node steps down.
    Status _rebuildStatus = Status::OK();  // (M)

    // The term that this service is running under.
    long long _term = OpTime::kUninitializedTerm;  // (M)

    // Map of running instances, keyed by InstanceID.
    SimpleBSONObjUnorderedMap<ActiveInstance> _activeInstances;  // (M)

    // A set of OpCtxs running on Client threads associated with this PrimaryOnlyService.
    stdx::unordered_set<OperationContext*> _opCtxs;  // (M)

    // CancellationSource used on stepdown/shutdown to cancel work in all running instances of a
    // PrimaryOnlyService.
    CancellationSource _source;
};

/**
 * Registry that contains all PrimaryOnlyServices.  Services should be registered at process start
 * up before accepting connections, and the set of registered services should not change from that
 * point on.
 * The Registry is responsible for notifying all PrimaryOnlyServices of replication state changes
 * into and out of primary state.
 */
class PrimaryOnlyServiceRegistry final : public ReplicaSetAwareService<PrimaryOnlyServiceRegistry> {
public:
    PrimaryOnlyServiceRegistry() = default;
    ~PrimaryOnlyServiceRegistry() = default;

    static PrimaryOnlyServiceRegistry* get(ServiceContext* serviceContext);

    /**
     * Registers a new PrimaryOnlyService. Should only be called at startup.
     */
    void registerService(std::unique_ptr<PrimaryOnlyService> service);

    /**
     * Looks up a registered service by service name.  Calling it with a non-registered service name
     * is a programmer error as all services should be known statically and registered at startup.
     * Since all services live for the lifetime of the mongod process (unlike their Instance
     * objects), there's no concern about the returned pointer becoming invalid.
     */
    PrimaryOnlyService* lookupServiceByName(StringData serviceName);

    /**
     * Looks up a registered service by the namespace of its state document collection. Returns
     * nullptr if no service is found with the given state document namespace.
     */
    PrimaryOnlyService* lookupServiceByNamespace(const NamespaceString& ns);

    /**
     * Adds a 'primaryOnlyServices' sub-obj to the 'result' BSONObjBuilder containing information
     * (given by PrimaryService::reportForServerStatus) of each registered service.
     */
    void reportServiceInfoForServerStatus(BSONObjBuilder* result) noexcept;

    /**
     * Adds information about the Instances running in all registered services to 'ops', to show up
     * in currentOp(). 'connMode' and 'sessionMode' are arguments provided to currentOp, and can be
     * used to make decisions about whether or not various Instances should actually show up in the
     * currentOp() output for this invocation of currentOp().
     */
    void reportServiceInfoForCurrentOp(MongoProcessInterface::CurrentOpConnectionsMode connMode,
                                       MongoProcessInterface::CurrentOpSessionsMode sessionMode,
                                       std::vector<BSONObj>* ops) noexcept;

    void onStartup(OperationContext*) final;
    void onInitialDataAvailable(OperationContext* opCtx, bool isMajorityDataAvailable) final {}
    void onShutdown() final;
    void onStepUpBegin(OperationContext*, long long term) final {}
    void onBecomeArbiter() final {}
    void onStepUpComplete(OperationContext*, long long term) final;
    void onStepDown() final;

private:
    StringMap<std::unique_ptr<PrimaryOnlyService>> _servicesByName;

    // Doesn't own the service, contains a pointer to the service owned by _servicesByName.
    // This is safe since services don't change after startup.
    StringMap<PrimaryOnlyService*> _servicesByNamespace;
};

}  // namespace repl
}  // namespace mongo

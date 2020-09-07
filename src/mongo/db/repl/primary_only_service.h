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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
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
     * same conceptual task coexisting.
     * NOTE: PrimaryOnlyService implementations shouldn't have their Instance subclass extended this
     * Instance class directly, instead they should extend TypedInstance, defined below.
     */
    class Instance {
    public:
        friend class PrimaryOnlyService;

        virtual ~Instance() = default;

        SharedSemiFuture<void> getCompletionFuture() {
            return _completionPromise.getFuture();
        }

    protected:
        /**
         * This is the main function that PrimaryOnlyService implementations will need to implement,
         * and is where the bulk of the work those services perform is scheduled. All work run for
         * this Instance *must* be scheduled on 'executor'. Instances are responsible for inserting,
         * updating, and deleting their state documents as needed.
         */
        virtual SemiFuture<void> run(
            std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept = 0;

    private:
        bool _running = false;

        // Promise that gets emplaced when the future returned by run() resolves.
        SharedPromise<void> _completionPromise;
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
        static boost::optional<std::shared_ptr<InstanceType>> lookup(PrimaryOnlyService* service,
                                                                     const InstanceID& id) {
            auto instance = service->lookupInstance(id);
            if (!instance) {
                return boost::none;
            }

            return checked_pointer_cast<InstanceType>(instance.get());
        }

        /**
         * Same functionality as PrimaryOnlyService::getOrCreateInstance, but returns a pointer of
         * the proper derived class for the Instance.
         */
        static std::shared_ptr<InstanceType> getOrCreate(PrimaryOnlyService* service,
                                                         BSONObj initialState) {
            auto instance = service->getOrCreateInstance(std::move(initialState));
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
     * _instances and deleted.
     */
    void releaseInstance(const InstanceID& id);

    /**
     * Releases all Instances from _instances. Called by the OpObserver if this service's state
     * document collection is dropped.
     */
    void releaseAllInstances();

    /**
     * Returns whether this service is currently running.  This is true only when the node is in
     * state PRIMARY *and* this service has finished all asynchronous work associated with resuming
     * after stepUp.
     */
    bool isRunning() const;

    /**
     * Returns the number of currently running Instances of this service.
     */
    size_t getNumberOfInstances();

protected:
    /**
     * Constructs a new Instance object with the given initial state.
     */
    virtual std::shared_ptr<Instance> constructInstance(BSONObj initialState) const = 0;

    /**
     * Given an InstanceId returns the corresponding running Instance object, or boost::none if
     * there is none.
     */
    boost::optional<std::shared_ptr<Instance>> lookupInstance(const InstanceID& id);

    /**
     * Extracts an InstanceID from the _id field of the given 'initialState' object. If an Instance
     * with the extracted InstanceID already exists in _intances, returns it.  If not, constructs a
     * new Instance (by calling constructInstance()), registers it in _instances, and returns it.
     * It is illegal to call this more than once with 'initialState' documents that have the same
     * _id but are otherwise not completely identical.
     * Throws NotWritablePrimary if the node is not currently primary.
     */
    std::shared_ptr<Instance> getOrCreateInstance(BSONObj initialState);

private:
    /**
     * Called as part of onStepUp.  Queries the state document collection for this
     * PrimaryOnlyService, constructs Instance objects for each document found, and schedules work
     * to run all the newly recreated Instances.
     */
    void _rebuildInstances() noexcept;

    /**
     * Schedules work to call the provided instance's 'run' method. Must be called while holding
     * _mutex.
     */
    void _scheduleRun(WithLock, std::shared_ptr<Instance> instance);

    ServiceContext* const _serviceContext;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("PrimaryOnlyService::_mutex");

    // Condvar to receive notifications when _rebuildInstances has completed after stepUp.
    stdx::condition_variable _rebuildCV;

    // A ScopedTaskExecutor that is used to perform all work run on behalf of an Instance.
    // This ScopedTaskExecutor wraps _executor and is created at stepUp and destroyed at
    // stepDown so that all outstanding tasks get interrupted.
    std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;

    // The concrete TaskExecutor backing _scopedExecutor. While _scopedExecutor is created and
    // destroyed with each stepUp/stepDown, _executor persists for the lifetime of the process. We
    // want _executor to survive failover to prevent us from having to reallocate lots of
    // thread and connection resources on every stepUp. Service instances should never have access
    // to _executor directly, they should only ever use _scopedExecutor so that they get the
    // guarantee that all outstanding tasks are interrupted at stepDown.
    std::shared_ptr<executor::TaskExecutor> _executor;

    enum class State {
        kRunning,
        kPaused,
        kRebuilding,
        kRebuildFailed,
        kShutdown,
    };

    State _state = State::kPaused;

    // If reloading the state documents from disk fails, this Status gets set to a non-ok value and
    // calls to lookup() or getOrCreate() will throw this status until the node steps down.
    Status _rebuildStatus = Status::OK();

    // The term that this service is running under.
    long long _term = OpTime::kUninitializedTerm;

    // Map of running instances, keyed by InstanceID.
    using InstanceMap = SimpleBSONObjUnorderedMap<std::shared_ptr<Instance>>;
    InstanceMap _instances;
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
     * Adds a 'primaryOnlyServices' sub-obj to the 'result' BSONObjBuilder containing a count of the
     * number of active instances for each registered service.
     */
    void reportServiceInfo(BSONObjBuilder* result);

    void onStartup(OperationContext*) final;
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

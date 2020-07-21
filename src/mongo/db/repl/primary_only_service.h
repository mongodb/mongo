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
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/string_map.h"

namespace mongo {

class OperationContext;
class ServiceContext;

namespace repl {

extern FailPoint PrimaryOnlyServiceHangBeforeCreatingInstance;

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
        struct RunOnceResult {
            enum State {
                kKeepRunning,
                kFinished,
            };
            // If set to kFinished signals that the task this Instance represents is complete, and
            // both the in-memory Instance object and the on-disk state document can be cleaned up.
            State state = kKeepRunning;

            // If set, signals that the next call to 'runOnce' on this instance shouldn't occur
            // until this optime is majority committed.
            boost::optional<OpTime> optime;

            static RunOnceResult kComplete() {
                return RunOnceResult{kFinished, boost::none};
            }

            static RunOnceResult kKeepGoing(OpTime ot) {
                return RunOnceResult{kKeepRunning, std::move(ot)};
            }

        private:
            RunOnceResult(State st, boost::optional<OpTime> ot)
                : state(std::move(st)), optime(std::move(ot)) {}
        };

        virtual ~Instance() = default;

        /**
         * This is the main function that PrimaryOnlyService implementations will need to implement,
         * and is where the bulk of the work those services perform is executed. The
         * PrimaryOnlyService machinery will call this function repeatedly until the RunOnceResult
         * returned has 'complete' set to true, at which point the state document will be deleted
         * and the Instance destroyed.
         */
        virtual SemiFuture<RunOnceResult> runOnce(OperationContext* opCtx) = 0;
    };

    /**
     * Extends Instance with a template specifier that subclasses should specialize on
     * themselves. This allows providing a function to lookup an Instance object that returns the
     * proper derived Instance type.
     */
    template <class InstanceType>
    class TypedInstance : public Instance {
    public:
        TypedInstance() = default;
        virtual ~TypedInstance() = default;

        /**
         * Same functionality as PrimaryOnlyService::lookupInstanceBase, but returns a pointer of
         * the proper derived class for the Instance.
         */
        static boost::optional<std::shared_ptr<InstanceType>> lookup(PrimaryOnlyService* service,
                                                                     const InstanceID& id) {
            auto instance = service->lookupInstanceBase(id);
            if (!instance) {
                return boost::none;
            }

            return checked_pointer_cast<InstanceType>(instance.get());
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
     * Virtual shutdown method where derived services can put their specific shutdown logic.
     */
    virtual void shutdownImpl() = 0;

    /**
     * Releases all running Instances, then shuts down and joins _executor, ensuring that there are
     * no remaining tasks running.
     * Ends by calling shutdownImpl so that derived services can clean up their local state.
     */
    void shutdown();

    /**
     * Called on transition to primary. Resumes any running Instances of this service
     * based on their persisted state documents. Also joins() any outstanding jobs from the previous
     * term, thereby ensuring that two Instance objects with the same InstanceID cannot coexist.
     */
    void onStepUp(long long term);

    /**
     * Called on stepDown. Releases all running Instances of this service from management by this
     * PrimaryOnlyService object. The Instances will have their OperationContexts interrupted
     * independently. Instance objects may continue to exist in memory in a detached state until the
     * next stepUp. Also shuts down _executor, forcing all outstanding jobs to complete.
     */
    void onStepDown();

    /**
     * Writes the given 'initialState' object to the service's state document collection and then
     * schedules work to construct an in-memory instance object and start it running, as soon as the
     * write of the state object is majority committed.  This is the main way that consumers can
     * start up new PrimaryOnlyService tasks.
     */
    SemiFuture<InstanceID> startNewInstance(OperationContext* opCtx, BSONObj initialState);

protected:
    /**
     * Returns a ScopedTaskExecutor used to run instances of this service.
     */
    virtual std::unique_ptr<executor::ScopedTaskExecutor> getTaskExecutor() = 0;

    /**
     * Constructs a new Instance object with the given initial state.
     */
    virtual std::shared_ptr<Instance> constructInstance(const BSONObj& initialState) const = 0;

    /**
     * Given an InstanceId returns the corresponding running Instance object, or boost::none if
     * there is none. Note that 'lookupInstanceBase' will not find a newly added Instance until the
     * Future returned by the call to 'startNewInstance' that added it resolves.
     */
    boost::optional<std::shared_ptr<Instance>> lookupInstanceBase(const InstanceID& id);

private:
    ServiceContext* const _serviceContext;

    Mutex _mutex = MONGO_MAKE_LATCH("PrimaryOnlyService::_mutex");

    // A ScopedTaskExecutor that is used to schedule calls to runOnce against Instance objects.
    // PrimaryOnlyService implementations are responsible for creating a TaskExecutor configured
    // with the desired options.  The size of the thread pool within the TaskExecutor limits the
    // number of Instances of this PrimaryOnlyService that can be actively running on a thread
    // simultaneously (though it does not limit the number of Instance objects that can
    // simultaneously exist).
    // This ScopedTaskExecutor wraps the TaskExecutor owned by the PrimaryOnlyService
    // implementation, and is created at stepUp and destroyed at stepDown so that all outstanding
    // tasks get interrupted.
    std::unique_ptr<executor::ScopedTaskExecutor> _executor;

    enum class State {
        kRunning,
        kPaused,
        kShutdown,
    };

    State _state = State::kRunning;

    // The term that this service is running under.
    long long _term = OpTime::kUninitializedTerm;

    // Map of running instances, keyed by InstanceID.
    SimpleBSONObjUnorderedMap<std::shared_ptr<Instance>> _instances;
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
     * Looks up a registered service.  Calling it with a non-registered service name is a programmer
     * error as all services should be known statically and registered at startup. Since all
     * services live for the lifetime of the mongod process (unlike their Instance objects), there's
     * no concern about the returned pointer becoming invalid.
     */
    PrimaryOnlyService* lookupService(StringData serviceName);

    /**
     * Shuts down all registered services.
     */
    void shutdown();

    void onStepUpBegin(OperationContext*, long long term) final {}
    void onBecomeArbiter() final{};
    void onStepUpComplete(OperationContext*, long long term) final;
    void onStepDown() final;

private:
    StringMap<std::unique_ptr<PrimaryOnlyService>> _services;
};

}  // namespace repl
}  // namespace mongo

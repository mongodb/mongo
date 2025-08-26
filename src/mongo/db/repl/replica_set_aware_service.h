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

#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * ReplicaSetAwareServices are those that need to respond to changes in per-node replication state,
 * such as becoming primary, stepping down, and entering rollback.
 *
 * Using this interface avoids the need to manually hook the various places in
 * ReplicationCoordinatorExternalStateImpl where these events occur.
 *
 * To define a ReplicaSetAwareService, a class needs to:
 *
 * 1. Inherit from ReplicaSetAwareService (templated on itself).
 * 2. Implement the pure virtual methods in ReplicaSetAwareInterface.
 * 3. Store a singleton object of the class somewhere (ideally as a ServiceContext decoration).
 * 4. Define a public static `get(ServiceContext*)` function.
 * 5. Define a static ReplicaSetAwareServiceRegistry::Registerer object to declare the name (and
 *    optionally pre-requisite services) of the service.
 *
 * If the service should only be active in certain configurations, then the class should override
 * shouldRegisterReplicaSetAwareService.  For the common cases of services that are only active on
 * config servers, the class can inherit from ReplicaSetAwareServiceConfigSvr (instead of
 * ReplicaSetAwareService).  Similarly, shard-only services can inherit from
 * ReplicaSetAwareServiceShardSvr.
 *
 * Example:
 *
 * #include "mongo/db/replica_set_aware_service.h"
 *
 * class FooService : public ReplicaSetAwareService<FooService> {
 * public:
 *     static FooService* get(ServiceContext* serviceContext);
 *
 *     // ...
 *
 * private:
 *     // Optional:
 *     virtual bool shouldRegisterReplicaSetAwareService() const final {
 *         return ...;
 *     }
 *
 *     // Mandatory:
 *     void onStartup(OperationContext* opCtx) final {
 *         // ...
 *     }
 *     void onSetCurrentConfig(OperationContext* opCtx) final {
 *         // ...
 *     }
 *     void onShutdown() final {
 *         // ...
 *     }
 *     void onStepUpBegin(OperationContext* opCtx) final {
 *         // ...
 *     }
 *     void onStepUpComplete(OperationContext* opCtx) final {
 *         // ...
 *     }
 *     void onStepDown() final {
 *         // ...
 *     }
 *     void onRollbackBegin() final {
 *         // ...
 *     }
 *     void onBecomeArbiter() final {
 *         // ...
 *     }
 * };
 *
 * namespace {
 *
 * const auto _fooDecoration = ServiceContext::declareDecoration<FooService>();
 *
 * const ReplicaSetAwareServiceRegistry::Registerer<FooService> _fooServiceRegisterer("FooService");
 *
 * }  // namespace
 *
 * FooService* FooService::get(ServiceContext* serviceContext) {
 *     return _fooDecoration(serviceContext);
 * }
 */

/**
 * Main API implemented by each ReplicaSetAwareService.
 */
class MONGO_MOD_PUB ReplicaSetAwareInterface {
public:
    /**
     * Called once during ReplicationCoordinator startup. A place to put startup logic such as
     * initializing thread pools. Cannot depend on the ReplicaSetConfig being loaded yet. Database
     * reads and writes to unreplicated collections are permitted.
     */
    virtual void onStartup(OperationContext* opCtx) = 0;

    /**
     * Called when the ReplicationCoordinator sets its replica set config, e.g. after processing
     * replSetInitiate, reconfiguring via heartbeat, or processing replSetReconfig. May be called
     * multiple times and not necessarily in the order the configs were processed.
     */
    virtual void onSetCurrentConfig(OperationContext* opCtx) = 0;

    /**
     * Called when a consistent (to a point-in-time) copy of data is available. That's:
     *   - After replSetInitiate
     *   - After initial sync completes (for both logical and file-copy based initial sync)
     *   - After rollback to the stable timestamp
     *   - After storage startup from a stable checkpoint
     *   - After replication recovery from an unstable checkpoint
     * Local reads are allowed at this point, with no special restrictions on resource locks. After
     * this function is called, all writes in the system are guaranteed to be against a consistent
     * copy of data. In other words, ReplicationCoordinator::isDataConsistent is guaranteed to
     * return true after this is called. Thus, in-memory states may choose to reconstruct here and
     * subscribe to future writes from now on (e.g. via OpObservers).
     *
     * If the "isMajority" flag is set, the data read locally is also committed
     * to a majority of replica set members. In the opposite case, the local data may be subject to
     * rollback attempts. Currently, completing a logical initial sync is the only case
     * "isMajority" is false because the initial syncing node may sync past the commit
     * point.
     *
     * If the "isRollback" flag is set, it means that this is called after storage rollback to
     * stable (but before oplog recovery to the common point). Note that, this function can be
     * invoked multiple times (due to replication rollbacks) in the lifetime of a mongod process but
     * it's guaranteed to be invoked only once with isRollback set to false. All subsequent
     * invocations of this function must have isRollback set to true.
     *
     * During rollbacks, onRollbackBegin is called first before onConsistentDataAvailable is
     * called. But onConsistentDataAvailable is called before OpObservers' onReplicationRollback
     * method is called. The difference is that onConsistentDataAvailable is called after storage
     * rollback to stable and OpObservers' onReplicationRollback method is called after we finish
     * oplog recovery to the common point. It is recommended to reconstruct in-memory states in
     * onConsistentDataAvailable and tail future writes for updates. But it is currently also a
     * pattern to reconstruct in-memory states using OpObservers' onReplicationRollback method. But
     * that is safe only if oplog recovery during rollback doesn't rely on those in-memory states.
     * See onReplicationRollback in op_observer.h for more details. (We have plans to consolidate
     * these two interfaces in the future but this is what we have for now.)
     */
    virtual void onConsistentDataAvailable(OperationContext* opCtx,
                                           bool isMajority,
                                           bool isRollback) = 0;

    /**
     * Called as part of ReplicationCoordinator shutdown.
     * Note that it is possible that we are still a writable primary after onShutdown() has been
     * called (see SERVER-81115).
     */
    virtual void onShutdown() = 0;

    /**
     * Called prior to stepping up as PRIMARY, i.e. after drain mode has completed but before
     * the RSTL is acquired.
     * Implementations of this method should be short-running in order to prevent blocking
     * the stepUp from completing.
     */
    virtual void onStepUpBegin(OperationContext* opCtx, long long term) = 0;

    /**
     * Called after the node has transitioned to PRIMARY, i.e. after stepUp reconfig and after
     * writing the first oplog entry with the new term, but before the node starts accepting
     * writes.
     * Implementations of this method should be short-running in order to prevent blocking
     * the stepUp from completing.
     */
    virtual void onStepUpComplete(OperationContext* opCtx, long long term) = 0;

    /**
     * Called after the node has transitioned out of PRIMARY. Usually this is into SECONDARY, but it
     * could also be into ROLLBACK or REMOVED.
     *
     * NB: also called when SECONDARY nodes transition to ROLLBACK, hence it should never be assumed
     * that `onStepUp` hooks have been invoked at least once before this method is invoked.
     */
    virtual void onStepDown() = 0;

    /**
     * Called after the node has transitioned to ROLLBACK.
     */
    virtual void onRollbackBegin() = 0;

    /**
     * Called when the node commences being an arbiter.
     */
    virtual void onBecomeArbiter() = 0;

    /**
     * Returns the name of the service. Used for logging purposes.
     */
    virtual std::string getServiceName() const = 0;
};


/**
 * The registry of ReplicaSetAwareServices.
 */
class MONGO_MOD_PUB ReplicaSetAwareServiceRegistry final : public ReplicaSetAwareInterface {
    ReplicaSetAwareServiceRegistry(const ReplicaSetAwareServiceRegistry&) = delete;
    ReplicaSetAwareServiceRegistry& operator=(const ReplicaSetAwareServiceRegistry&) = delete;

public:
    template <class ActualService>
    class Registerer {
        Registerer(const Registerer&) = delete;
        Registerer& operator=(const Registerer&) = delete;

    public:
        explicit Registerer(std::string name, std::vector<std::string> prereqs = {})
            : _registerer(
                  std::move(name),
                  std::move(prereqs),
                  [&](ServiceContext* serviceContext) {
                      if (!_registered) {
                          _registered = ActualService::get(serviceContext)->_shouldRegister();
                      }
                      if (*_registered) {
                          ReplicaSetAwareServiceRegistry::get(serviceContext)
                              ._registerService(ActualService::get(serviceContext));
                      }
                  },
                  [&](ServiceContext* serviceContext) {
                      if (_registered && *_registered) {
                          ReplicaSetAwareServiceRegistry::get(serviceContext)
                              ._unregisterService(ActualService::get(serviceContext));
                      }
                  }) {}

    private:
        boost::optional<bool> _registered;
        ServiceContext::ConstructorActionRegisterer _registerer;
    };

    ReplicaSetAwareServiceRegistry() = default;
    virtual ~ReplicaSetAwareServiceRegistry();

    static ReplicaSetAwareServiceRegistry& get(ServiceContext* serviceContext);

    void onStartup(OperationContext* opCtx) final;
    void onSetCurrentConfig(OperationContext* opCtx) final;
    void onConsistentDataAvailable(OperationContext* opCtx, bool isMajority, bool isRollback) final;
    void onShutdown() final;
    void onStepUpBegin(OperationContext* opCtx, long long term) final;
    void onStepUpComplete(OperationContext* opCtx, long long term) final;
    void onStepDown() final;
    void onRollbackBegin() final;
    void onBecomeArbiter() final;
    inline std::string getServiceName() const final {
        return "ReplicaSetAwareServiceRegistry";
    }

private:
    void _registerService(ReplicaSetAwareInterface* service);
    void _unregisterService(ReplicaSetAwareInterface* service);

    std::vector<ReplicaSetAwareInterface*> _services;
};


/**
 * The main ReplicaSetAwareService class that services inherit from.  Refer to the comment at the
 * start of this file for more detailed info.
 */
template <class ActualService>
class MONGO_MOD_OPEN ReplicaSetAwareService : private ReplicaSetAwareInterface {
    ReplicaSetAwareService(const ReplicaSetAwareService&) = delete;
    ReplicaSetAwareService& operator=(const ReplicaSetAwareService&) = delete;

public:
    virtual ~ReplicaSetAwareService() = default;

protected:
    ReplicaSetAwareService() = default;

private:
    friend ReplicaSetAwareServiceRegistry::Registerer<ActualService>;

    /**
     * Internal, called only by ReplicaSetAwareServiceRegistry::Registerer.
     */
    bool _shouldRegister() const {
        return shouldRegisterReplicaSetAwareService();
    }

    /**
     * Services should override this if they wish to only be registered in certain circumstances.
     * For the common cases of config/shard-server only, it's better to use the
     * ReplicaSetAwareServiceConfigSvr and ReplicaSetAwareServiceShardSvr convenience classes
     * defined below.
     */
    virtual bool shouldRegisterReplicaSetAwareService() const {
        return true;
    }
};


/**
 * Convenience version of ReplicaSetAwareService that is only active on config servers.
 */
template <class ActualService>
class MONGO_MOD_OPEN ReplicaSetAwareServiceConfigSvr
    : public ReplicaSetAwareService<ActualService> {
private:
    bool shouldRegisterReplicaSetAwareService() const final {
        return serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer);
    }
};


/**
 * Convenience version of ReplicaSetAwareService that is only active on shard servers.
 */
template <class ActualService>
class MONGO_MOD_OPEN ReplicaSetAwareServiceShardSvr : public ReplicaSetAwareService<ActualService> {
private:
    bool shouldRegisterReplicaSetAwareService() const final {
        return serverGlobalParams.clusterRole.has(ClusterRole::ShardServer);
    }
};

}  // namespace mongo

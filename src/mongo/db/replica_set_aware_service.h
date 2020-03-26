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

namespace mongo {

/**
 * ReplicaSetAwareServices are those that need to respond to changes in per-node replication state,
 * such as becoming primary, stepping down, and entering rollback.
 *
 * Using this interface avoids the need to manually hook the various places in
 * ReplicationCoordinatorExternalStateImpl where these events occur.
 *
 * To define a ReplicaSetAwareService, a class needs to inherit from ReplicaSetAwareService
 * (templated on itself), implement the pure virtual methods in ReplicaSetAwareInterface, and define
 * a static ReplicaSetAwareServiceRegistry::Registerer to declare the name (and optionally
 * pre-requisite services) of the service.
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
 *     // ...
 *
 * private:
 *     // Optional:
 *     virtual bool shouldRegisterReplicaSetAwareService() const final {
 *         return ...;
 *     }
 *
 *     // Mandatory:
 *     void onStepUpBegin(OperationContext* opCtx) final {
 *         // ...
 *     }
 *     void onStepUpComplete(OperationContext* opCtx) final {
 *         // ...
 *     }
 *     void onStepDown() final {
 *         // ...
 *     }
 * };
 *
 * namespace {
 *
 * ReplicaSetAwareServiceRegistry::Registerer<FooService> fooServiceRegisterer("FooService");
 *
 * }  // namespace
 */

/**
 * Main API implemented by each ReplicaSetAwareService.
 */
class ReplicaSetAwareInterface {
public:
    /**
     * Called prior to stepping up as PRIMARY, ie. after drain mode has completed.
     */
    virtual void onStepUpBegin(OperationContext* opCtx) = 0;

    /**
     * Called after the node has transitioned to PRIMARY.
     */
    virtual void onStepUpComplete(OperationContext* opCtx) = 0;

    /**
     * Called after the node has transitioned out of PRIMARY. Usually this is into SECONDARY, but it
     * could also be into ROLLBACK or REMOVED.
     */
    virtual void onStepDown() = 0;
};


/**
 * The registry of ReplicaSetAwareServices.
 */
class ReplicaSetAwareServiceRegistry final : public ReplicaSetAwareInterface {
    ReplicaSetAwareServiceRegistry(const ReplicaSetAwareServiceRegistry&) = delete;
    ReplicaSetAwareServiceRegistry& operator=(const ReplicaSetAwareServiceRegistry&) = delete;

public:
    template <class ActualService>
    class Registerer {
        Registerer(const Registerer&) = delete;
        Registerer& operator=(const Registerer&) = delete;

    public:
        explicit Registerer(std::string name, std::vector<std::string> prereqs = {})
            : _registerer(std::move(name),
                          std::move(prereqs),
                          [&](ServiceContext* serviceContext) {
                              if (!_registered) {
                                  _registered =
                                      ActualService::get(serviceContext)->_shouldRegister();
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

    void onStepUpBegin(OperationContext* opCtx) final;
    void onStepUpComplete(OperationContext* opCtx) final;
    void onStepDown() final;

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
class ReplicaSetAwareService : private ReplicaSetAwareInterface {
    ReplicaSetAwareService(const ReplicaSetAwareService&) = delete;
    ReplicaSetAwareService& operator=(const ReplicaSetAwareService&) = delete;

public:
    virtual ~ReplicaSetAwareService() = default;

    /**
     * Retrieves the per-serviceContext instance of the ActualService.
     */
    static ActualService* get(ServiceContext* serviceContext) {
        return &_decoration(serviceContext);
    }

    static ActualService* get(OperationContext* operationContext) {
        return get(operationContext->getServiceContext());
    }

protected:
    ReplicaSetAwareService() = default;

    /**
     * Used when services need to get a reference to the serviceContext that they are decorating.
     */
    ServiceContext* getServiceContext() {
        auto* actualService = checked_cast<ActualService*>(this);
        return &_decoration.owner(*actualService);
    }

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

    // The decoration of the actual service on the ServiceContext.
    // Definition of this static can't be inline, because it isn't constexpr.
    static Decorable<ServiceContext>::Decoration<ActualService> _decoration;
};

template <class ActualService>
Decorable<ServiceContext>::Decoration<ActualService>
    ReplicaSetAwareService<ActualService>::_decoration =
        ServiceContext::declareDecoration<ActualService>();


/**
 * Convenience version of ReplicaSetAwareService that is only active on config servers.
 */
template <class ActualService>
class ReplicaSetAwareServiceConfigSvr : public ReplicaSetAwareService<ActualService> {
private:
    virtual bool shouldRegisterReplicaSetAwareService() const final {
        return serverGlobalParams.clusterRole == ClusterRole::ConfigServer;
    }
};


/**
 * Convenience version of ReplicaSetAwareService that is only active on shard servers.
 */
template <class ActualService>
class ReplicaSetAwareServiceShardSvr : public ReplicaSetAwareService<ActualService> {
private:
    virtual bool shouldRegisterReplicaSetAwareService() const final {
        return serverGlobalParams.clusterRole == ClusterRole::ShardServer;
    }
};

}  // namespace mongo

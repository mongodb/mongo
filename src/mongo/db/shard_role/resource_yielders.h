// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/resource_yielder.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class [[MONGO_MOD_OPEN]] ResourceYielderFactory {
public:
    virtual ~ResourceYielderFactory() = default;

    static const std::unique_ptr<mongo::ResourceYielderFactory>& get(const Service& svc);
    static void set(Service& svc, std::unique_ptr<ResourceYielderFactory> implementation);

    static void initialize(ServiceContext* svcCtx);

    virtual std::unique_ptr<ResourceYielder> make(OperationContext* opCtx,
                                                  std::string_view cmdName) const = 0;
};

class [[MONGO_MOD_FILE_PRIVATE]] ShardResourceYielderFactory : public ResourceYielderFactory {
public:
    std::unique_ptr<ResourceYielder> make(OperationContext* opCtx,
                                          std::string_view cmdName) const override {
        if (opCtx->isActiveTransactionParticipant() && opCtx->inMultiDocumentTransaction()) {
            return TransactionParticipantResourceYielder::make(cmdName);
        } else {
            return TransactionRouterResourceYielder::makeForRemoteCommand();
        }
    }
};

class [[MONGO_MOD_PUBLIC]] RouterResourceYielderFactory : public ResourceYielderFactory {
public:
    std::unique_ptr<ResourceYielder> make(OperationContext* opCtx,
                                          std::string_view cmdName) const override {
        return TransactionRouterResourceYielder::makeForRemoteCommand();
    }
};

[[MONGO_MOD_PUBLIC]] inline void ResourceYielderFactory::initialize(ServiceContext* svcCtx) {
    if (auto svc = svcCtx->getService(); svc && svc->role().has(ClusterRole::ShardServer)) {
        ResourceYielderFactory::set(*svc, std::make_unique<ShardResourceYielderFactory>());
    }

    if (auto svc = svcCtx->getService(); svc && svc->role().has(ClusterRole::RouterServer)) {
        ResourceYielderFactory::set(*svc, std::make_unique<RouterResourceYielderFactory>());
    }
};

}  // namespace mongo

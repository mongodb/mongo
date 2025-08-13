/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/s/transaction_router_resource_yielder.h"

#include <memory>

namespace mongo {

class ResourceYielderFactory {
public:
    virtual ~ResourceYielderFactory() = default;

    static const std::unique_ptr<mongo::ResourceYielderFactory>& get(const Service& svc);
    static void set(Service& svc, std::unique_ptr<ResourceYielderFactory> implementation);

    static void initialize(ServiceContext* svcCtx);

    virtual std::unique_ptr<ResourceYielder> make(OperationContext* opCtx,
                                                  StringData cmdName) const = 0;
};

class ShardResourceYielderFactory : public ResourceYielderFactory {
public:
    std::unique_ptr<ResourceYielder> make(OperationContext* opCtx,
                                          StringData cmdName) const override {
        if (opCtx->isActiveTransactionParticipant() && opCtx->inMultiDocumentTransaction()) {
            return TransactionParticipantResourceYielder::make(cmdName);
        } else {
            return TransactionRouterResourceYielder::makeForRemoteCommand();
        }
    }
};

class RouterResourceYielderFactory : public ResourceYielderFactory {
public:
    std::unique_ptr<ResourceYielder> make(OperationContext* opCtx,
                                          StringData cmdName) const override {
        return TransactionRouterResourceYielder::makeForRemoteCommand();
    }
};

inline void ResourceYielderFactory::initialize(ServiceContext* svcCtx) {
    if (auto svc = svcCtx->getService(ClusterRole::ShardServer)) {
        ResourceYielderFactory::set(*svc, std::make_unique<ShardResourceYielderFactory>());
    }

    if (auto svc = svcCtx->getService(ClusterRole::RouterServer)) {
        ResourceYielderFactory::set(*svc, std::make_unique<RouterResourceYielderFactory>());
    }
};

}  // namespace mongo

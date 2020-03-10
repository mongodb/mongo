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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/replica_set_aware_service.h"

namespace mongo {

namespace {

auto registryDecoration = ServiceContext::declareDecoration<ReplicaSetAwareServiceRegistry>();

}  // namespace

ReplicaSetAwareServiceRegistry::~ReplicaSetAwareServiceRegistry() {
    invariant(_services.empty());
}

ReplicaSetAwareServiceRegistry& ReplicaSetAwareServiceRegistry::get(
    ServiceContext* serviceContext) {
    return registryDecoration(serviceContext);
}

void ReplicaSetAwareServiceRegistry::onStepUpBegin(OperationContext* opCtx) {
    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        service->onStepUpBegin(opCtx);
    });
}

void ReplicaSetAwareServiceRegistry::onStepUpComplete(OperationContext* opCtx) {
    std::for_each(_services.begin(), _services.end(), [&](ReplicaSetAwareInterface* service) {
        service->onStepUpComplete(opCtx);
    });
}

void ReplicaSetAwareServiceRegistry::onStepDown() {
    std::for_each(_services.begin(), _services.end(), [](ReplicaSetAwareInterface* service) {
        service->onStepDown();
    });
}

void ReplicaSetAwareServiceRegistry::_registerService(ReplicaSetAwareInterface* service) {
    _services.push_back(service);
}

void ReplicaSetAwareServiceRegistry::_unregisterService(ReplicaSetAwareInterface* service) {
    auto it = std::find(_services.begin(), _services.end(), service);
    invariant(it != _services.end());
    _services.erase(it);
}

}  // namespace mongo

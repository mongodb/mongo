// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

#include "mongo/db/storage/durable_history_pin.h"

#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
const auto getDurableHistoryRegistry =
    ServiceContext::declareDecoration<std::unique_ptr<DurableHistoryRegistry>>();

}  // namespace

DurableHistoryRegistry* DurableHistoryRegistry::get(ServiceContext* service) {
    return getDurableHistoryRegistry(service).get();
}

DurableHistoryRegistry* DurableHistoryRegistry::get(ServiceContext& service) {
    return getDurableHistoryRegistry(service).get();
}

DurableHistoryRegistry* DurableHistoryRegistry::get(OperationContext* opCtx) {
    return get(opCtx->getClient()->getServiceContext());
}

void DurableHistoryRegistry::set(ServiceContext* service,
                                 std::unique_ptr<DurableHistoryRegistry> registry) {
    auto& decoratedRegistry = getDurableHistoryRegistry(service);
    decoratedRegistry = std::move(registry);
}

void DurableHistoryRegistry::registerPin(std::unique_ptr<DurableHistoryPin> pin) {
    _pins.push_back(std::move(pin));
}

void DurableHistoryRegistry::reconcilePins(OperationContext* opCtx) {
    StorageEngine* engine = opCtx->getServiceContext()->getStorageEngine();
    if (!engine->supportsRecoveryTimestamp()) {
        return;
    }

    for (auto& pin : _pins) {
        boost::optional<Timestamp> pinTs = pin->calculatePin(opCtx);
        LOGV2_FOR_RECOVERY(5384102,
                           2,
                           "Reconciling timestamp pin.",
                           "name"_attr = pin->getName(),
                           "ts"_attr = pinTs);
        if (pinTs) {
            auto swTimestamp = engine->pinOldestTimestamp(
                *shard_role_details::getRecoveryUnit(opCtx), pin->getName(), pinTs.value(), false);
            if (!swTimestamp.isOK()) {
                LOGV2_WARNING(5384105,
                              "Unable to repin oldest timestamp",
                              "service"_attr = pin->getName(),
                              "request"_attr = pinTs.value(),
                              "error"_attr = swTimestamp.getStatus());
            }
        } else {
            engine->unpinOldestTimestamp(pin->getName());
        }
    }
}

void DurableHistoryRegistry::clearPins(OperationContext* opCtx) {
    StorageEngine* engine = opCtx->getServiceContext()->getStorageEngine();
    if (!engine->supportsRecoveryTimestamp()) {
        return;
    }

    for (auto& pin : _pins) {
        engine->unpinOldestTimestamp(pin->getName());
    }
}

}  // namespace mongo

/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include "mongo/db/storage/durable_history_pin.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

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
            auto swTimestamp =
                engine->pinOldestTimestamp(opCtx, pin->getName(), pinTs.get(), false);
            if (!swTimestamp.isOK()) {
                LOGV2_WARNING(5384105,
                              "Unable to repin oldest timestamp",
                              "service"_attr = pin->getName(),
                              "request"_attr = pinTs.get(),
                              "error"_attr = swTimestamp.getStatus());
            }
        } else {
            engine->unpinOldestTimestamp(pin->getName());
        }
    }
}

}  // namespace mongo

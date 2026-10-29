// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/storage_interface.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {
namespace repl {


namespace {
const auto getStorageInterface =
    ServiceContext::declareDecoration<std::unique_ptr<StorageInterface>>();
}

StorageInterface* StorageInterface::get(ServiceContext* service) {
    return getStorageInterface(service).get();
}

StorageInterface* StorageInterface::get(ServiceContext& service) {
    return getStorageInterface(service).get();
}

StorageInterface* StorageInterface::get(OperationContext* opCtx) {
    return get(opCtx->getClient()->getServiceContext());
}

void StorageInterface::set(ServiceContext* service, std::unique_ptr<StorageInterface> storage) {
    auto& storageInterface = getStorageInterface(service);
    storageInterface = std::move(storage);
}

}  // namespace repl
}  // namespace mongo

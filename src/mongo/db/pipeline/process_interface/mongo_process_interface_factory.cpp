// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/process_interface/mongo_process_interface_factory.h"

namespace mongo {
std::shared_ptr<MongoProcessInterface> MongoProcessInterfaceFactoryImpl::create(
    OperationContext* opCtx) {
    return MongoProcessInterface::create(opCtx);
}

}  // namespace mongo

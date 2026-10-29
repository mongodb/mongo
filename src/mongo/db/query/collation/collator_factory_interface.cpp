// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collator_factory_interface.h"

#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {

const auto getCollatorFactory =
    ServiceContext::declareDecoration<std::unique_ptr<CollatorFactoryInterface>>();

}  // namespace

CollatorFactoryInterface* CollatorFactoryInterface::get(ServiceContext* serviceContext) {
    invariant(getCollatorFactory(serviceContext));
    return getCollatorFactory(serviceContext).get();
}

void CollatorFactoryInterface::set(ServiceContext* serviceContext,
                                   std::unique_ptr<CollatorFactoryInterface> collatorFactory) {
    getCollatorFactory(serviceContext) = std::move(collatorFactory);
}

}  // namespace mongo

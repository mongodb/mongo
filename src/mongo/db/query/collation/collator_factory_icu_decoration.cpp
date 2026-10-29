// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/query/collation/collator_factory_icu.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <string>

namespace mongo {
namespace {
ServiceContext::ConstructorActionRegisterer registerIcuCollator{
    "CreateCollatorFactory", {"LoadICUData"}, [](ServiceContext* service) {
        CollatorFactoryInterface::set(service, std::make_unique<CollatorFactoryICU>());
    }};
}  // namespace
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/resource_yielders.h"

namespace mongo {

namespace {
const auto getFactory = Service::declareDecoration<std::unique_ptr<ResourceYielderFactory>>();
}  // namespace

const std::unique_ptr<mongo::ResourceYielderFactory>& ResourceYielderFactory::get(
    const Service& svc) {
    return svc[getFactory];
}

void ResourceYielderFactory::set(Service& svc,
                                 std::unique_ptr<ResourceYielderFactory> implementation) {
    svc[getFactory] = std::move(implementation);
}

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_service_dependencies.h"

namespace mongo::query_settings {
namespace {
const auto dependenciesDecoration = ServiceContext::declareDecoration<ServiceDependencies>();
}  // namespace

ServiceDependencies& getServiceDependencies(ServiceContext* serviceContext) {
    return dependenciesDecoration(serviceContext);
}
}  // namespace mongo::query_settings

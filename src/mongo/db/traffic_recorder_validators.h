// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

Status validateTrafficRecordDestination(const std::string& path, const boost::optional<TenantId>&);


}  // namespace mongo

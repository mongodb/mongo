// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/util/functional.h"

namespace mongo::transport {

Status launchServiceWorkerThread(unique_function<void()> task);

}  // namespace mongo::transport

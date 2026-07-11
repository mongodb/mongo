// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/session_manager_common.h"

namespace mongo {

/**
 * mongod uses the common implementation of SessionManager without modifications.
 */
using SessionManagerMongod = transport::SessionManagerCommon;

}  // namespace mongo

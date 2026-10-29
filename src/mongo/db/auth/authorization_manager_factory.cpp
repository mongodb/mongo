// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_manager_factory.h"


namespace mongo {

std::unique_ptr<AuthorizationManagerFactory> globalAuthzManagerFactory;

}  // namespace mongo

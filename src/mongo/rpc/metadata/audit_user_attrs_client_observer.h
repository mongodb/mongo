// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional.hpp>

namespace mongo::audit {
class AuditUserAttrsClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) final {};
    void onDestroyClient(Client* client) final {};

    void onCreateOperationContext(OperationContext* opCtx) final;
    void onDestroyOperationContext(OperationContext* opCtx) final {};
};

}  // namespace mongo::audit

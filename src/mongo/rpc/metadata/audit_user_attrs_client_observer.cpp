// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/audit_user_attrs_client_observer.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/util/decorable.h"

namespace mongo::audit {
namespace {
ServiceContext::ConstructorActionRegisterer auditUserAttrsClientObserverRegisterer{
    "AuditUserAttrsClientObserverRegisterer", [](ServiceContext* svcCtx) {
        svcCtx->registerClientObserver(std::make_unique<AuditUserAttrsClientObserver>());
    }};

}  // namespace

void AuditUserAttrsClientObserver::onCreateOperationContext(OperationContext* opCtx) {
    rpc::AuditUserAttrs::resetToAuthenticatedUser(opCtx);
}

}  // namespace mongo::audit

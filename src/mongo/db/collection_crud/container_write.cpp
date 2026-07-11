// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/collection_crud/container_write.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/version_context.h"

namespace mongo::container_write {
namespace {
void assertCanAcceptContainerWrites(OperationContext* opCtx) {
    const auto fcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    uassert(ErrorCodes::InvalidOptions,
            "Container write support is not enabled",
            rss::ReplicatedStorageService::get(opCtx->getServiceContext())
                    .getPersistenceProvider()
                    .mustUseContainerWrites() ||
                ::mongo::feature_flags::gContainerWrites.isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(opCtx), fcv));

    uassert(ErrorCodes::NotWritablePrimary,
            str::stream() << "Not primary while writing to container",
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(
                opCtx, NamespaceString::kContainerNamespace));
}
}  // namespace

Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value,
              boost::optional<NonexistentKeyGuarantee> nkg) {
    assertCanAcceptContainerWrites(opCtx);
    auto status = container.insert(ru,
                                   key,
                                   value,
                                   nkg ? container::ExistingKeyPolicy::overwrite
                                       : container::ExistingKeyPolicy::reject);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerInsert(
        opCtx, container.ident()->getIdent(), key, value);

    return Status::OK();
}

Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key,
              std::span<const char> value,
              boost::optional<NonexistentKeyGuarantee> nkg) {
    assertCanAcceptContainerWrites(opCtx);
    auto status = container.insert(ru,
                                   key,
                                   value,
                                   nkg ? container::ExistingKeyPolicy::overwrite
                                       : container::ExistingKeyPolicy::reject);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerInsert(
        opCtx, container.ident()->getIdent(), key, value);

    return Status::OK();
}

Status update(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value) {
    assertCanAcceptContainerWrites(opCtx);
    auto status = container.update(ru, key, value);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerUpdate(
        opCtx, container.ident()->getIdent(), key, value);

    return Status::OK();
}

Status update(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key,
              std::span<const char> value) {
    assertCanAcceptContainerWrites(opCtx);
    auto status = container.update(ru, key, value);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerUpdate(
        opCtx, container.ident()->getIdent(), key, value);

    return Status::OK();
}

Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key) {
    assertCanAcceptContainerWrites(opCtx);
    auto status = container.remove(ru, key);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerDelete(
        opCtx, container.ident()->getIdent(), key);

    return Status::OK();
}

Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key) {
    assertCanAcceptContainerWrites(opCtx);
    auto status = container.remove(ru, key);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerDelete(
        opCtx, container.ident()->getIdent(), key);

    return Status::OK();
}

}  // namespace mongo::container_write

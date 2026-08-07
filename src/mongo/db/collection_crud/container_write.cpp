// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/collection_crud/container_write.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/version_context.h"

#include <algorithm>
#include <tuple>

namespace mongo::container_write {
CanAcceptContainerWritesGuarantee CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(
    OperationContext* opCtx) {
    uassert(ErrorCodes::InvalidOptions,
            "Container write support is not enabled",
            rss::ReplicatedStorageService::get(opCtx->getServiceContext())
                    .getPersistenceProvider()
                    .mustUseContainerWrites() ||
                ::mongo::feature_flags::gContainerWrites.isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    uassert(ErrorCodes::NotWritablePrimary,
            str::stream() << "Not primary while writing to container",
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(
                opCtx, NamespaceString::kContainerNamespace));
    return CanAcceptContainerWritesGuarantee{};
}

Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value,
              boost::optional<CanAcceptContainerWritesGuarantee> wg,
              boost::optional<NonexistentKeyGuarantee> nkg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
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
              boost::optional<CanAcceptContainerWritesGuarantee> wg,
              boost::optional<NonexistentKeyGuarantee> nkg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
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
              IntegerKeyedContainer& container,
              std::span<const int64_t> keys,
              std::span<const std::span<const char>> values,
              boost::optional<CanAcceptContainerWritesGuarantee> wg,
              boost::optional<NonexistentKeyGuarantee> nkg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
    massert(13274502,
            "Spans for keys and values must have the same size",
            keys.size() == values.size());
    auto status = container.insert(ru,
                                   keys,
                                   values,
                                   nkg ? container::ExistingKeyPolicy::overwrite
                                       : container::ExistingKeyPolicy::reject);
    if (!status.isOK()) {
        return status;
    }

    auto* opObserver = opCtx->getServiceContext()->getOpObserver();
    auto ident = container.ident()->getIdent();

    // The storage write above is batched -- the container reuses one cursor for the whole range --
    // but the oplog entries are not.
    // TODO SERVER-133068 emit batched oplog entries directly
    for (size_t i = 0; i < keys.size(); ++i) {
        opObserver->onContainerInsert(opCtx, ident, keys[i], values[i]);
    }

    return Status::OK();
}

Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const std::span<const char>> keys,
              std::span<const std::span<const char>> values,
              boost::optional<CanAcceptContainerWritesGuarantee> wg,
              boost::optional<NonexistentKeyGuarantee> nkg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
    massert(13274503,
            "Spans for keys and values must have the same size",
            keys.size() == values.size());
    auto status = container.insert(ru,
                                   keys,
                                   values,
                                   nkg ? container::ExistingKeyPolicy::overwrite
                                       : container::ExistingKeyPolicy::reject);
    if (!status.isOK()) {
        return status;
    }

    auto* opObserver = opCtx->getServiceContext()->getOpObserver();
    auto ident = container.ident()->getIdent();

    // The storage write above is batched -- the container reuses one cursor for the whole range --
    // but the oplog entries are not.
    // TODO SERVER-133068 emit batched oplog entries directly
    for (size_t i = 0; i < keys.size(); ++i) {
        opObserver->onContainerInsert(opCtx, ident, keys[i], values[i]);
    }

    return Status::OK();
}

Status update(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value,
              boost::optional<CanAcceptContainerWritesGuarantee> wg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
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
              std::span<const char> value,
              boost::optional<CanAcceptContainerWritesGuarantee> wg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
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
              int64_t key,
              boost::optional<CanAcceptContainerWritesGuarantee> wg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
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
              std::span<const char> key,
              boost::optional<CanAcceptContainerWritesGuarantee> wg) {
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
    auto status = container.remove(ru, key);
    if (!status.isOK()) {
        return status;
    }

    opCtx->getServiceContext()->getOpObserver()->onContainerDelete(
        opCtx, container.ident()->getIdent(), key);

    return Status::OK();
}

}  // namespace mongo::container_write

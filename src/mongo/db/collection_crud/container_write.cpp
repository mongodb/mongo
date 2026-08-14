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
    if (keys.empty() && values.empty()) {
        return Status::OK();  // Early exit if empty
    }
    massert(13274502,
            "Spans for keys and values must have the same size",
            keys.size() == values.size());
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
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

    size_t runStart = 0;
    for (size_t i = 1; i <= keys.size(); ++i) {
        const bool finalRun = i == keys.size();
        // Emit batched oplog entry on the final iteration, or when a non-contiguous key is found
        if (finalRun || keys[i] != keys[i - 1] + 1) {
            opObserver->onContainerInsert(
                opCtx, ident, keys[runStart], values.subspan(runStart, i - runStart));
            runStart = i;
        }
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
    if (keys.empty() && values.empty()) {
        return Status::OK();  // Early exit if empty
    }
    massert(13274503,
            "Spans for keys and values must have the same size",
            keys.size() == values.size());
    if (!wg) {
        std::ignore = CanAcceptContainerWritesGuarantee::assertCanAcceptContainerWrites(opCtx);
    }
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

    size_t runStart = 0;
    for (size_t i = 1; i <= keys.size(); ++i) {
        const bool finalRun = i == keys.size();
        // Emit batched oplog entry on final iteration, or when the key changes
        if (finalRun ||
            !std::equal(values[i].begin(),
                        values[i].end(),
                        values[runStart].begin(),
                        values[runStart].end())) {
            opObserver->onContainerInsert(
                opCtx, ident, keys.subspan(runStart, i - runStart), values[runStart]);
            runStart = i;
        }
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

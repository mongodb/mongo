/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/read_write_concern_defaults.h"

#include "mongo/db/logical_clock.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

static constexpr auto kReadConcernLevelsDisallowedAsDefault = {
    repl::ReadConcernLevel::kSnapshotReadConcern, repl::ReadConcernLevel::kLinearizableReadConcern};

const auto getReadWriteConcernDefaults =
    ServiceContext::declareDecoration<boost::optional<ReadWriteConcernDefaults>>();

ServiceContext::ConstructorActionRegisterer destroyReadWriteConcernDefaultsRegisterer(
    "DestroyReadWriteConcernDefaults",
    [](ServiceContext* service) {
        // Intentionally empty, since construction happens through different code paths depending on
        // the binary
    },
    [](ServiceContext* service) { getReadWriteConcernDefaults(service).reset(); });

}  // namespace

bool ReadWriteConcernDefaults::isSuitableReadConcernLevel(repl::ReadConcernLevel level) {
    for (auto bannedLevel : kReadConcernLevelsDisallowedAsDefault) {
        if (level == bannedLevel) {
            return false;
        }
    }
    return true;
}

void ReadWriteConcernDefaults::checkSuitabilityAsDefault(const ReadConcern& rc) {
    uassert(ErrorCodes::BadValue,
            str::stream() << "level: '" << repl::readConcernLevels::toString(rc.getLevel())
                          << "' is not suitable for the default read concern",
            isSuitableReadConcernLevel(rc.getLevel()));
    uassert(ErrorCodes::BadValue,
            str::stream() << "'" << ReadConcern::kAfterOpTimeFieldName
                          << "' is not suitable for the default read concern",
            !rc.getArgsOpTime());
    uassert(ErrorCodes::BadValue,
            str::stream() << "'" << ReadConcern::kAfterClusterTimeFieldName
                          << "' is not suitable for the default read concern",
            !rc.getArgsAfterClusterTime());
    uassert(ErrorCodes::BadValue,
            str::stream() << "'" << ReadConcern::kAtClusterTimeFieldName
                          << "' is not suitable for the default read concern",
            !rc.getArgsAtClusterTime());
    uassert(ErrorCodes::BadValue,
            str::stream() << "'" << ReadWriteConcernProvenance::kSourceFieldName
                          << "' must be unset in default read concern",
            !rc.getProvenance().hasSource());
}

void ReadWriteConcernDefaults::checkSuitabilityAsDefault(const WriteConcern& wc) {
    uassert(ErrorCodes::BadValue,
            "Unacknowledged write concern is not suitable for the default write concern",
            !(wc.wMode.empty() && wc.wNumNodes < 1));
    uassert(ErrorCodes::BadValue,
            str::stream() << "'" << ReadWriteConcernProvenance::kSourceFieldName
                          << "' must be unset in default write concern",
            !wc.getProvenance().hasSource());
}

RWConcernDefault ReadWriteConcernDefaults::generateNewConcerns(
    OperationContext* opCtx,
    const boost::optional<ReadConcern>& rc,
    const boost::optional<WriteConcern>& wc) {
    uassert(ErrorCodes::BadValue,
            str::stream() << "At least one of the \""
                          << RWConcernDefault::kDefaultReadConcernFieldName << "\" or \""
                          << RWConcernDefault::kDefaultWriteConcernFieldName
                          << "\" fields must be present",
            rc || wc);

    RWConcernDefault rwc;
    if (rc && !rc->isEmpty()) {
        checkSuitabilityAsDefault(*rc);
        rwc.setDefaultReadConcern(rc);
    }
    if (wc && !wc->usedDefault) {
        checkSuitabilityAsDefault(*wc);
        rwc.setDefaultWriteConcern(wc);
    }

    auto* const serviceContext = opCtx->getServiceContext();
    rwc.setUpdateOpTime(LogicalClock::get(serviceContext)->getClusterTime().asTimestamp());
    rwc.setUpdateWallClockTime(serviceContext->getFastClockSource()->now());

    auto current = _getDefault(opCtx);
    if (!rc && current) {
        rwc.setDefaultReadConcern(current->getDefaultReadConcern());
    }
    if (!wc && current) {
        rwc.setDefaultWriteConcern(current->getDefaultWriteConcern());
    }

    return rwc;
}

void ReadWriteConcernDefaults::observeDirectWriteToConfigSettings(OperationContext* opCtx,
                                                                  BSONElement idElem,
                                                                  boost::optional<BSONObj> newDoc) {
    if (idElem.str() != kPersistedDocumentId) {
        // The affected document wasn't the read write concern defaults document.
        return;
    }

    // Note this will throw if the document can't be parsed. In the case of a delete, there will be
    // no new defaults document and the RWConcern will be default constructed, which matches the
    // behavior when lookup discovers a non-existent defaults document.
    auto newDefaultsDoc = newDoc
        ? RWConcernDefault::parse(IDLParserErrorContext("RWDefaultsWriteObserver"),
                                  newDoc->getOwned())
        : RWConcernDefault();

    opCtx->recoveryUnit()->onCommit([this, opCtx, newDefaultsDoc = std::move(newDefaultsDoc)](
                                        boost::optional<Timestamp> unusedCommitTime) mutable {
        setDefault(opCtx, std::move(newDefaultsDoc));
    });
}

void ReadWriteConcernDefaults::invalidate() {
    _defaults.invalidate(Type::kReadWriteConcernEntry);
}

void ReadWriteConcernDefaults::setDefault(OperationContext* opCtx, RWConcernDefault&& rwc) {
    _defaults.insertOrAssignAndGet(Type::kReadWriteConcernEntry,
                                   std::move(rwc),
                                   opCtx->getServiceContext()->getFastClockSource()->now());
}

void ReadWriteConcernDefaults::refreshIfNecessary(OperationContext* opCtx) {
    auto possibleNewDefaults = _defaults.lookup(opCtx, Type::kReadWriteConcernEntry);
    if (!possibleNewDefaults) {
        return;
    }

    auto currentDefaultsHandle = _defaults.acquire(opCtx, Type::kReadWriteConcernEntry);
    if (!currentDefaultsHandle || !possibleNewDefaults->getUpdateOpTime() ||
        (possibleNewDefaults->getUpdateOpTime() > currentDefaultsHandle->getUpdateOpTime())) {
        // Use the new defaults if they have a higher epoch, if there are no defaults in the cache,
        // or if the found defaults have no epoch, meaning there are no defaults in config.settings.
        LOGV2(20997,
              "refreshed RWC defaults to {newDefaults}",
              "newDefaults"_attr = possibleNewDefaults->toBSON());
        setDefault(opCtx, std::move(*possibleNewDefaults));
    }
}

boost::optional<ReadWriteConcernDefaults::RWConcernDefaultAndTime>
ReadWriteConcernDefaults::_getDefault(OperationContext* opCtx) {
    auto defaultsHandle = _defaults.acquire(opCtx, Type::kReadWriteConcernEntry);
    if (defaultsHandle) {
        // Since CWRWC is ok with continuing to use a value well after it has been invalidated
        // (since RWC defaults apply for the lifetime of the op/cursor), we don't need to check
        // defaultsValue.isValid() here, and we don't need to return the Handle, since callers don't
        // need to check defaultsValue.isValid() later, either.  Just dereference it to get the
        // underlying contents.
        return RWConcernDefaultAndTime(*defaultsHandle, defaultsHandle.updateWallClockTime());
    }
    return boost::none;
}

boost::optional<ReadWriteConcernDefaults::ReadConcern>
ReadWriteConcernDefaults::getDefaultReadConcern(OperationContext* opCtx) {
    auto current = getDefault(opCtx);
    return current.getDefaultReadConcern();
}

boost::optional<ReadWriteConcernDefaults::WriteConcern>
ReadWriteConcernDefaults::getDefaultWriteConcern(OperationContext* opCtx) {
    auto current = getDefault(opCtx);
    return current.getDefaultWriteConcern();
}

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(ServiceContext* service) {
    return *getReadWriteConcernDefaults(service);
}

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(OperationContext* opCtx) {
    return *getReadWriteConcernDefaults(opCtx->getServiceContext());
}

void ReadWriteConcernDefaults::create(ServiceContext* service, FetchDefaultsFn fetchDefaultsFn) {
    getReadWriteConcernDefaults(service).emplace(service, fetchDefaultsFn);
}

ReadWriteConcernDefaults::ReadWriteConcernDefaults(ServiceContext* service,
                                                   FetchDefaultsFn fetchDefaultsFn)
    : _defaults(service,
                _threadPool,
                [fetchDefaultsFn = std::move(fetchDefaultsFn)](
                    OperationContext* opCtx, const Type&) { return fetchDefaultsFn(opCtx); }),
      _threadPool([] {
          ThreadPool::Options options;
          options.poolName = "ReadWriteConcernDefaults";
          options.minThreads = 0;
          options.maxThreads = 1;

          return options;
      }()) {
    _threadPool.startup();
}

ReadWriteConcernDefaults::~ReadWriteConcernDefaults() = default;

ReadWriteConcernDefaults::Cache::Cache(ServiceContext* service,
                                       ThreadPoolInterface& threadPool,
                                       LookupFn lookupFn)
    : ReadThroughCache(_mutex, service, threadPool, 1 /* cacheSize */),
      _lookupFn(std::move(lookupFn)) {}

boost::optional<RWConcernDefault> ReadWriteConcernDefaults::Cache::lookup(
    OperationContext* opCtx, const ReadWriteConcernDefaults::Type& key) {
    invariant(key == Type::kReadWriteConcernEntry);
    return _lookupFn(opCtx, key);
}

}  // namespace mongo

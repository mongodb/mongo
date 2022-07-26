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


#include "mongo/platform/basic.h"

#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


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
            !wc.isUnacknowledged());
    uassert(ErrorCodes::BadValue,
            str::stream() << "'" << ReadWriteConcernProvenance::kSourceFieldName
                          << "' must be unset in default write concern",
            !wc.getProvenance().hasSource());
}

RWConcernDefault ReadWriteConcernDefaults::generateNewCWRWCToBeSavedOnDisk(
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
    if (wc && !wc->usedDefaultConstructedWC) {
        checkSuitabilityAsDefault(*wc);
        rwc.setDefaultWriteConcern(wc);
    }

    auto* const serviceContext = opCtx->getServiceContext();
    const auto currentTime = VectorClock::get(serviceContext)->getTime();
    rwc.setUpdateOpTime(currentTime.clusterTime().asTimestamp());
    rwc.setUpdateWallClockTime(serviceContext->getFastClockSource()->now());

    auto current = _getDefaultCWRWCFromDisk(opCtx);
    if (!rc && current) {
        rwc.setDefaultReadConcern(current->getDefaultReadConcern());
    }
    if (!wc && current) {
        rwc.setDefaultWriteConcern(current->getDefaultWriteConcern());
    }
    // If the setDefaultRWConcern command tries to unset the global default write concern when it
    // has already been set, throw an error.
    // wc->usedDefaultConstructedWC indicates that the defaultWriteConcern given in the
    // setDefaultRWConcern command was empty (i.e. {defaultWriteConcern: {}})
    // If current->getDefaultWriteConcern exists, that means the global default write concern has
    // already been set.
    if (wc && wc->usedDefaultConstructedWC && current) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "The global default write concern cannot be unset once it is set.",
                !current->getDefaultWriteConcern());
    }

    return rwc;
}

bool ReadWriteConcernDefaults::isCWWCSet(OperationContext* opCtx) {
    return getCWWC(opCtx) ? true : false;
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
        ? RWConcernDefault::parse(IDLParserContext("RWDefaultsWriteObserver"), newDoc->getOwned())
        : RWConcernDefault();

    opCtx->recoveryUnit()->onCommit([this, opCtx, newDefaultsDoc = std::move(newDefaultsDoc)](
                                        boost::optional<Timestamp> unusedCommitTime) mutable {
        setDefault(opCtx, std::move(newDefaultsDoc));
    });
}

void ReadWriteConcernDefaults::invalidate() {
    _defaults.invalidateKey(Type::kReadWriteConcernEntry);
}

void ReadWriteConcernDefaults::setDefault(OperationContext* opCtx, RWConcernDefault&& rwc) {
    _defaults.insertOrAssignAndGet(Type::kReadWriteConcernEntry,
                                   std::move(rwc),
                                   opCtx->getServiceContext()->getFastClockSource()->now());
}

void ReadWriteConcernDefaults::refreshIfNecessary(OperationContext* opCtx) {
    auto possibleNewDefaults = _defaults.lookup(opCtx);
    if (!possibleNewDefaults) {
        return;
    }

    auto currentDefaultsHandle = _defaults.acquire(opCtx, Type::kReadWriteConcernEntry);
    if (!currentDefaultsHandle || !possibleNewDefaults->getUpdateOpTime() ||
        (possibleNewDefaults->getUpdateOpTime() > currentDefaultsHandle->getUpdateOpTime())) {
        // Use the new defaults if they have a higher epoch, if there are no defaults in the cache,
        // or if the found defaults have no epoch, meaning there are no defaults in config.settings.
        auto possibleNewDefaultsBSON = possibleNewDefaults->toBSON();
        auto defaultsBefore = currentDefaultsHandle ? *currentDefaultsHandle : RWConcernDefault();

        setDefault(opCtx, std::move(*possibleNewDefaults));

        auto postUpdateDefaultsHandle = _defaults.acquire(opCtx, Type::kReadWriteConcernEntry);
        auto defaultsAfter =
            postUpdateDefaultsHandle ? *postUpdateDefaultsHandle : RWConcernDefault();

        // Log only if we updated the read- or write-concern defaults themselves.
        if (defaultsBefore.getDefaultWriteConcern() != defaultsAfter.getDefaultWriteConcern() ||
            (defaultsBefore.getDefaultReadConcern() && defaultsAfter.getDefaultReadConcern() &&
             (defaultsBefore.getDefaultReadConcern().get().getLevel() !=
              defaultsAfter.getDefaultReadConcern().get().getLevel()))) {
            LOGV2(20997, "Refreshed RWC defaults", "newDefaults"_attr = possibleNewDefaultsBSON);
        }
    }
}

repl::ReadConcernArgs ReadWriteConcernDefaults::getImplicitDefaultReadConcern() {
    return repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
}

boost::optional<ReadWriteConcernDefaults::RWConcernDefaultAndTime>
ReadWriteConcernDefaults::_getDefaultCWRWCFromDisk(OperationContext* opCtx) {
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

ReadWriteConcernDefaults::RWConcernDefaultAndTime ReadWriteConcernDefaults::getDefault(
    OperationContext* opCtx) {
    auto cached = _getDefaultCWRWCFromDisk(opCtx).value_or(RWConcernDefaultAndTime());

    // Only overwrite the default read concern and its source if it has already been set on mongos.
    if (!cached.getDefaultReadConcernSource()) {
        if (!cached.getDefaultReadConcern() || cached.getDefaultReadConcern().get().isEmpty()) {
            auto rcDefault = getImplicitDefaultReadConcern();
            cached.setDefaultReadConcern(rcDefault);
            cached.setDefaultReadConcernSource(DefaultReadConcernSourceEnum::kImplicit);
        } else {
            cached.setDefaultReadConcernSource(DefaultReadConcernSourceEnum::kGlobal);
        }
    }

    // If the config hasn't yet been loaded on the node, the default will be w:1, since we have no
    // way of calculating the implicit default. This means that after we have loaded our config,
    // nodes could change their implicit write concern default. This is safe since we shouldn't be
    // accepting writes that need a write concern before we have loaded our config.
    // This prevents overriding the default write concern and its source on mongos if it has
    // already been set through the config server.
    if (!cached.getDefaultWriteConcernSource()) {
        const bool isCWWCSet = cached.getDefaultWriteConcern() &&
            !cached.getDefaultWriteConcern().get().usedDefaultConstructedWC;
        if (isCWWCSet) {
            cached.setDefaultWriteConcernSource(DefaultWriteConcernSourceEnum::kGlobal);
        } else {
            cached.setDefaultWriteConcernSource(DefaultWriteConcernSourceEnum::kImplicit);
            if (_implicitDefaultWriteConcernMajority &&
                _implicitDefaultWriteConcernMajority.get()) {
                cached.setDefaultWriteConcern(
                    WriteConcernOptions(WriteConcernOptions::kMajority,
                                        WriteConcernOptions::SyncMode::UNSET,
                                        WriteConcernOptions::kNoTimeout));
            }
        }
    }

    return cached;
}

void ReadWriteConcernDefaults::setImplicitDefaultWriteConcernMajority(
    bool newImplicitDefaultWCMajority) {
    invariant(!_implicitDefaultWriteConcernMajority ||
              repl::enableDefaultWriteConcernUpdatesForInitiate.load());
    _implicitDefaultWriteConcernMajority = newImplicitDefaultWCMajority;
}

boost::optional<bool> ReadWriteConcernDefaults::getImplicitDefaultWriteConcernMajority_forTest() {
    return _implicitDefaultWriteConcernMajority;
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

boost::optional<ReadWriteConcernDefaults::WriteConcern> ReadWriteConcernDefaults::getCWWC(
    OperationContext* opCtx) {
    auto cached = _getDefaultCWRWCFromDisk(opCtx);
    if (cached && cached.get().getDefaultWriteConcern() &&
        !cached.get().getDefaultWriteConcern().get().usedDefaultConstructedWC) {
        return cached.get().getDefaultWriteConcern().get();
    }

    return boost::none;
}

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(ServiceContext* service) {
    return *getReadWriteConcernDefaults(service);
}

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(OperationContext* opCtx) {
    return *getReadWriteConcernDefaults(opCtx->getServiceContext());
}

void ReadWriteConcernDefaults::create(ServiceContext* service, FetchDefaultsFn fetchDefaultsFn) {
    getReadWriteConcernDefaults(service).emplace(service, std::move(fetchDefaultsFn));
}

ReadWriteConcernDefaults::ReadWriteConcernDefaults(ServiceContext* service,
                                                   FetchDefaultsFn fetchDefaultsFn)
    : _defaults(service, _threadPool, std::move(fetchDefaultsFn)),
      _threadPool([] {
          ThreadPool::Options options;
          options.poolName = "ReadWriteConcernDefaults";
          options.minThreads = 0;
          options.maxThreads = 1;

          return options;
      }()),
      _implicitDefaultWriteConcernMajority(boost::none) {
    _threadPool.startup();
}

ReadWriteConcernDefaults::~ReadWriteConcernDefaults() = default;

ReadWriteConcernDefaults::Cache::Cache(ServiceContext* service,
                                       ThreadPoolInterface& threadPool,
                                       FetchDefaultsFn fetchDefaultsFn)
    : ReadThroughCache(_mutex,
                       service,
                       threadPool,
                       [this](OperationContext* opCtx, Type, const ValueHandle& unusedCachedValue) {
                           return LookupResult(lookup(opCtx));
                       },
                       1 /* cacheSize */),
      _fetchDefaultsFn(std::move(fetchDefaultsFn)) {}

boost::optional<RWConcernDefault> ReadWriteConcernDefaults::Cache::lookup(OperationContext* opCtx) {
    return _fetchDefaultsFn(opCtx);
}

}  // namespace mongo

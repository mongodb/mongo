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
#include "mongo/util/log.h"

namespace mongo {

namespace {

static constexpr auto kReadConcernLevelsDisallowedAsDefault = {
    repl::ReadConcernLevel::kSnapshotReadConcern, repl::ReadConcernLevel::kLinearizableReadConcern};

/**
 * Used to invalidate the cache on updates to the persisted defaults document.
 */
class OnUpdateCommitHandler final : public RecoveryUnit::Change {
public:
    // rwcDefaults must outlive instantiations of this class.
    OnUpdateCommitHandler(ServiceContext* service,
                          ReadWriteConcernDefaults* rwcDefaults,
                          const boost::optional<BSONObj>& newDefaultsDoc)
        : _service(service), _rwcDefaults(rwcDefaults) {
        // Note this will throw if the document can't be parsed. In the case of a delete, there will
        // be no new defaults document and the RWConcern will be default constructed, which  matches
        // the behavior when lookup discovers a non-existent defaults document.
        _rwc = newDefaultsDoc
            ? RWConcernDefault::parse(IDLParserErrorContext("RWDefaultsWriteObserver"),
                                      *newDefaultsDoc)
            : RWConcernDefault();
    }

    void commit(boost::optional<Timestamp> timestamp) final {
        _rwc.setLocalSetTime(_service->getFastClockSource()->now());
        _rwcDefaults->setDefault(std::move(_rwc));
    }

    void rollback() final {}

private:
    ServiceContext* _service;
    ReadWriteConcernDefaults* _rwcDefaults;
    RWConcernDefault _rwc;
};

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
}

void ReadWriteConcernDefaults::checkSuitabilityAsDefault(const WriteConcern& wc) {
    uassert(ErrorCodes::BadValue,
            "Unacknowledged write concern is not suitable for the default write concern",
            !(wc.wMode.empty() && wc.wNumNodes < 1));
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
    if (wc && !wc->usedDefaultW) {
        checkSuitabilityAsDefault(*wc);
        rwc.setDefaultWriteConcern(wc);
    }
    auto epoch = LogicalClock::get(opCtx->getServiceContext())->getClusterTime().asTimestamp();
    rwc.setEpoch(epoch);
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    rwc.setSetTime(now);

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

    opCtx->recoveryUnit()->registerChange(
        std::make_unique<OnUpdateCommitHandler>(opCtx->getServiceContext(), this, newDoc));
}

void ReadWriteConcernDefaults::invalidate() {
    _defaults.invalidate(Type::kReadWriteConcernEntry);
}

void ReadWriteConcernDefaults::setDefault(RWConcernDefault&& rwc) {
    _defaults.revalidate(Type::kReadWriteConcernEntry, std::move(rwc));
}

void ReadWriteConcernDefaults::refreshIfNecessary(OperationContext* opCtx) {
    auto possibleNewDefaults = _defaults.lookup(opCtx, Type::kReadWriteConcernEntry);
    if (!possibleNewDefaults) {
        return;
    }
    auto currentDefaultsHandle = _defaults.acquire(opCtx, Type::kReadWriteConcernEntry);
    if (!currentDefaultsHandle ||
        (possibleNewDefaults->getEpoch() > (**currentDefaultsHandle)->getEpoch())) {
        // Use the new defaults if they have a higher epoch, or if there are currently no defaults.
        log() << "refreshed RWC defaults to " << possibleNewDefaults->toBSON();
        setDefault(std::move(*possibleNewDefaults));
    }
}

boost::optional<RWConcernDefault> ReadWriteConcernDefaults::_getDefault(OperationContext* opCtx) {
    auto defaultsHandle = _defaults.acquire(opCtx, Type::kReadWriteConcernEntry);
    if (defaultsHandle) {
        auto& defaultsValue = **defaultsHandle;
        // Since CWRWC is ok with continuing to use a value well after it has been invalidated
        // (since RWC defaults apply for the lifetime of the op/cursor), we don't need to check
        // defaultsValue.isValid() here, and we don't need to return the Handle, since callers don't
        // need to check defaultsValue.isValid() later, either.  Just dereference it to get the
        // underlying contents.
        return *defaultsValue;
    }
    return boost::none;
}

RWConcernDefault ReadWriteConcernDefaults::getDefault(OperationContext* opCtx) {
    return _getDefault(opCtx).value_or(RWConcernDefault());
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

namespace {

const auto getReadWriteConcernDefaults =
    ServiceContext::declareDecoration<std::unique_ptr<ReadWriteConcernDefaults>>();

}  // namespace

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(ServiceContext* service) {
    return *getReadWriteConcernDefaults(service);
}

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(ServiceContext& service) {
    return *getReadWriteConcernDefaults(service);
}

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(OperationContext* opCtx) {
    return *getReadWriteConcernDefaults(opCtx->getServiceContext());
}

void ReadWriteConcernDefaults::create(ServiceContext* service, LookupFn lookupFn) {
    getReadWriteConcernDefaults(service) = std::make_unique<ReadWriteConcernDefaults>(lookupFn);
}

ReadWriteConcernDefaults::ReadWriteConcernDefaults(LookupFn lookupFn) : _defaults(lookupFn) {}

ReadWriteConcernDefaults::Cache::Cache(LookupFn lookupFn)
    : DistCache(1, _mutex), _lookupFn(lookupFn) {}

boost::optional<RWConcernDefault> ReadWriteConcernDefaults::Cache::lookup(
    OperationContext* opCtx, const ReadWriteConcernDefaults::Type& key) {
    invariant(key == Type::kReadWriteConcernEntry);
    auto newDefaults = _lookupFn(opCtx);
    if (newDefaults) {
        newDefaults->setLocalSetTime(opCtx->getServiceContext()->getFastClockSource()->now());
    }
    return newDefaults;
}

}  // namespace mongo

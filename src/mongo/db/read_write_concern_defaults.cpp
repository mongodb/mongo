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

#include "mongo/db/logical_clock.h"

namespace mongo {

void ReadWriteConcernDefaults::checkSuitabilityAsDefault(const ReadConcern& rc) {
    uassert(ErrorCodes::BadValue,
            str::stream() << "level: '" << ReadConcern::kSnapshotReadConcernStr
                          << "' is not suitable for the default read concern",
            rc.getLevel() != repl::ReadConcernLevel::kSnapshotReadConcern);
    uassert(ErrorCodes::BadValue,
            str::stream() << "level: '" << ReadConcern::kLinearizableReadConcernStr
                          << "' is not suitable for the default read concern",
            rc.getLevel() != repl::ReadConcernLevel::kLinearizableReadConcern);
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

void ReadWriteConcernDefaults::_setDefault(WithLock, RWConcernDefault&& rwc) {
    _defaults.erase(kReadWriteConcernEntry);
    _defaults.emplace(kReadWriteConcernEntry, rwc);
}

RWConcernDefault ReadWriteConcernDefaults::setConcerns(OperationContext* opCtx,
                                                       const boost::optional<ReadConcern>& rc,
                                                       const boost::optional<WriteConcern>& wc) {
    invariant(rc || wc);

    if (rc) {
        checkSuitabilityAsDefault(*rc);
    }
    if (wc) {
        checkSuitabilityAsDefault(*wc);
    }

    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    auto epoch = LogicalClock::get(opCtx->getServiceContext())->getClusterTime().asTimestamp();

    RWConcernDefault rwc;
    rwc.setDefaultReadConcern(rc);
    rwc.setDefaultWriteConcern(wc);
    rwc.setEpoch(epoch);
    rwc.setSetTime(now);
    rwc.setLocalSetTime(now);

    stdx::lock_guard<Latch> lk(_mutex);

    auto current = _getDefault(lk);
    if (!rc && current) {
        rwc.setDefaultReadConcern(current->getDefaultReadConcern());
    }
    if (!wc && current) {
        rwc.setDefaultWriteConcern(current->getDefaultWriteConcern());
    }
    _setDefault(lk, std::move(rwc));
    return *_getDefault(lk);
}

void ReadWriteConcernDefaults::invalidate() {
    stdx::lock_guard<Latch> lk(_mutex);
    _defaults.erase(kReadWriteConcernEntry);
}

boost::optional<RWConcernDefault> ReadWriteConcernDefaults::_getDefault(WithLock) const {
    if (_defaults.find(kReadWriteConcernEntry) == _defaults.end()) {
        return boost::none;
    }
    return _defaults.at(kReadWriteConcernEntry);
}

RWConcernDefault ReadWriteConcernDefaults::getDefault() const {
    auto current = ([&]() {
        stdx::lock_guard<Latch> lk(_mutex);
        return _getDefault(lk);
    })();
    if (!current) {
        return RWConcernDefault{};
    }
    return *current;
}

boost::optional<ReadWriteConcernDefaults::ReadConcern>
ReadWriteConcernDefaults::getDefaultReadConcern() const {
    auto current = getDefault();
    return current.getDefaultReadConcern();
}

boost::optional<ReadWriteConcernDefaults::WriteConcern>
ReadWriteConcernDefaults::getDefaultWriteConcern() const {
    auto current = getDefault();
    return current.getDefaultWriteConcern();
}


namespace {

const auto getReadWriteConcernDefaults =
    ServiceContext::declareDecoration<ReadWriteConcernDefaults>();

}  // namespace

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(ServiceContext* service) {
    return getReadWriteConcernDefaults(service);
}

ReadWriteConcernDefaults& ReadWriteConcernDefaults::get(ServiceContext& service) {
    return getReadWriteConcernDefaults(service);
}

}  // namespace mongo

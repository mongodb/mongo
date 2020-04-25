/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"

#include <sstream>

namespace mongo {

//
// CappedInsertNotifier
//

void CappedInsertNotifier::notifyAll() {
    stdx::lock_guard<Latch> lk(_mutex);
    ++_version;
    _notifier.notify_all();
}

void CappedInsertNotifier::waitUntil(uint64_t prevVersion, Date_t deadline) const {
    stdx::unique_lock<Latch> lk(_mutex);
    while (!_dead && prevVersion == _version) {
        if (stdx::cv_status::timeout == _notifier.wait_until(lk, deadline.toSystemTimePoint())) {
            return;
        }
    }
}

void CappedInsertNotifier::kill() {
    stdx::lock_guard<Latch> lk(_mutex);
    _dead = true;
    _notifier.notify_all();
}

bool CappedInsertNotifier::isDead() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _dead;
}

// ----

namespace {
const auto getFactory = ServiceContext::declareDecoration<std::unique_ptr<Collection::Factory>>();
}

Collection::Factory* Collection::Factory::get(ServiceContext* service) {
    return getFactory(service).get();
}

Collection::Factory* Collection::Factory::get(OperationContext* opCtx) {
    return getFactory(opCtx->getServiceContext()).get();
};

void Collection::Factory::set(ServiceContext* service,
                              std::unique_ptr<Collection::Factory> newFactory) {
    auto& factory = getFactory(service);
    factory = std::move(newFactory);
}

// static
Status Collection::parseValidationLevel(StringData newLevel) {
    if (newLevel == "") {
        // default
        return Status::OK();
    } else if (newLevel == "off") {
        return Status::OK();
    } else if (newLevel == "moderate") {
        return Status::OK();
    } else if (newLevel == "strict") {
        return Status::OK();
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid validation level: " << newLevel);
    }
}

// static
Status Collection::parseValidationAction(StringData newAction) {
    if (newAction == "") {
        // default
        return Status::OK();
    } else if (newAction == "warn") {
        return Status::OK();
    } else if (newAction == "error") {
        return Status::OK();
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "invalid validation action: " << newAction);
    }
}

}  // namespace mongo

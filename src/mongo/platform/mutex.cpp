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

#include "mongo/platform/mutex.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo::latch_detail {

void Identity::serialize(BSONObjBuilder* bob) const {
    bob->append("name"_sd, name());

    size_t id = index();
    bob->appendNumber("latchId"_sd, id);

    auto& hal = level();
    invariant(hal);
    bob->appendNumber("level"_sd, hal->index());

    auto& loc = sourceLocation();
    invariant(loc);
    size_t line = loc->line();
    bob->append("file"_sd, loc->file_name());
    bob->appendNumber("line"_sd, line);
}

Mutex::Mutex(std::shared_ptr<Data> data) : _data{std::move(data)} {
    invariant(_data);

    _data->counts().created.fetchAndAdd(1);
}

Mutex::~Mutex() {
    invariant(!_isLocked, getName().toString());

    _data->counts().destroyed.fetchAndAdd(1);
}

void Mutex::lock() {
    if (_mutex.try_lock()) {
        _isLocked = true;
        _onQuickLock();
        return;
    }

    _onContendedLock();
    _mutex.lock();
    _isLocked = true;
    _onSlowLock();
}

void Mutex::unlock() {
    _onUnlock();
    _isLocked = false;
    _mutex.unlock();
}
bool Mutex::try_lock() {
    if (!_mutex.try_lock()) {
        return false;
    }

    _isLocked = true;
    _onQuickLock();
    return true;
}

StringData Mutex::getName() const {
    return StringData(_data->identity().name());
}

void Mutex::_onContendedLock() noexcept {
    _data->counts().contended.fetchAndAdd(1);

    auto& state = getDiagnosticListenerState();
    if (!state.isFinalized.load()) {
        return;
    }

    for (auto listener : state.listeners) {
        listener->onContendedLock(_data->identity());
    }
}

void Mutex::_onQuickLock() noexcept {
    _data->counts().acquired.fetchAndAdd(1);

    auto& state = getDiagnosticListenerState();
    if (!state.isFinalized.load()) {
        return;
    }

    for (auto listener : state.listeners) {
        listener->onQuickLock(_data->identity());
    }
}

void Mutex::_onSlowLock() noexcept {
    _data->counts().acquired.fetchAndAdd(1);

    auto& state = getDiagnosticListenerState();
    if (!state.isFinalized.load()) {
        return;
    }

    for (auto listener : state.listeners) {
        listener->onSlowLock(_data->identity());
    }
}

void Mutex::_onUnlock() noexcept {
    _data->counts().released.fetchAndAdd(1);

    auto& state = getDiagnosticListenerState();
    if (!state.isFinalized.load()) {
        return;
    }

    for (auto listener : state.listeners) {
        listener->onUnlock(_data->identity());
    }
}

/**
 * Any MONGO_INITIALIZER that adds a DiagnosticListener will want to list
 * FinalizeDiagnosticListeners as a dependent initializer. This means that all DiagnosticListeners
 * are certified to be added before main and no DiagnosticListeners are ever invoked before main.
 */
MONGO_INITIALIZER(FinalizeDiagnosticListeners)(InitializerContext* context) {
    auto& state = latch_detail::getDiagnosticListenerState();
    state.isFinalized.store(true);
}

}  // namespace mongo::latch_detail

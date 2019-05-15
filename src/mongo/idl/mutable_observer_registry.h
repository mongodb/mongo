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

#pragma once

#include <vector>

#include "mongo/base/status.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/functional.h"

namespace mongo {

/**
 * Offers a type which allows idl to register observers for server parameters at runtime.
 *
 * An easy way to leverage this type is to register it as an on_update handler via
 *   on_update: std::ref(someGlobalMutableObserverRegistry)
 *
 * Then adding observers via someGlobalMutableObserverRegistry.addObserver();
 */
template <typename T>
class MutableObserverRegistry {
public:
    using Argument = T;

    void addObserver(unique_function<void(const T&)> observer) {
        stdx::lock_guard lk(_mutex);
        _registry.emplace_back(std::move(observer));
    }

    // TODO SERVER-40224: remove the Status return when on_update is changed to return void
    Status operator()(const T& t) {
        stdx::lock_guard lk(_mutex);
        for (const auto& observer : _registry) {
            observer(t);
        }

        return Status::OK();
    }

private:
    stdx::mutex _mutex;
    std::vector<unique_function<void(const T&)>> _registry;
};

}  // namespace mongo

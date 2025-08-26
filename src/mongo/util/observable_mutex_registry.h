/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/string_map.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * The registry keeps track of all registered instances of `ObservableMutex` and provides the
 * interface to iterate through them in order to collect contention stats.
 */
class ObservableMutexRegistry {
public:
    // TODO SERVER-108695: modify the following typedef with public ObservationToken
    using CollectionCallback =
        std::function<void(StringData tag,
                           const Date_t& registered,
                           observable_mutex_details::ObservationToken& token)>;

    ObservableMutexRegistry() : ObservableMutexRegistry(SystemClockSource::get()) {}
    explicit ObservableMutexRegistry(ClockSource* clk) : _clockSource(clk) {}

    /**
     * This may have performance implications as it serializes registering
     * with lookups, so avoid using it for registering mutex objects that are
     * created on a hot path, with visible performance implications for the user.
     */
    template <typename MutexType>
    void add(StringData tag, const MutexType& mutex) {
#ifdef MONGO_CONFIG_MUTEX_OBSERVATION
        auto hashedTag = _mutexEntries.hash_function().hashed_key(tag);
        stdx::lock_guard lk(_mutex);
        _mutexEntries[hashedTag].emplace_back(_clockSource->now(), mutex.token());
#endif
    }

    /**
     * Iterates over all registered mutex objects and runs the provided callback without holding the
     * registry's mutex. Mutex objects with invalid tokens are removed from the registry but are
     * still visited in the same iteration. This is to allow consumers to know when a token becomes
     * invalid.
     */
    void iterate(CollectionCallback cb);

    static ObservableMutexRegistry& get();

    size_t getNumRegistered_forTest() const;

private:
    struct MutexEntry {
        Date_t registrationTime;
        std::shared_ptr<observable_mutex_details::ObservationToken> token;
    };

    ClockSource* _clockSource;

    mutable stdx::mutex _mutex;
    StringMap<std::list<MutexEntry>> _mutexEntries;
};

}  // namespace mongo

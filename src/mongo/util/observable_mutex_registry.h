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

#include "mongo/base/string_data.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * The registry keeps track of all registered instances of `ObservableMutex` and provides the
 * interface to iterate through them in order to collect contention stats.
 */
class ObservableMutexRegistry {
public:
    enum class TrackingMode { kAggregate, kSeparate };

    // TODO SERVER-108695: modify the following typedef with public ObservationToken
    using CollectionCallback =
        std::function<void(StringData, TrackingMode, observable_mutex_details::ObservationToken&)>;

    /**
     * This may have performance implications as it serializes registering
     * with lookups, so avoid using it for registering mutex objects that are
     * created on a hot path, with visible performance implications for the user.
     */
    template <typename MutexType>
    void add(StringData tag, const MutexType& mutex, TrackingMode mode = TrackingMode::kAggregate) {
#ifdef MONGO_CONFIG_MUTEX_OBSERVATION
        auto hashedTag = _mutexEntries.hash_function().hashed_key(tag);
        stdx::lock_guard lk(_mutex);
        auto& entries = _mutexEntries[hashedTag];

        iassert(ErrorCodes::InternalError,
                "Unable to register the same tag under different tracking modes",
                entries.empty() || entries.begin()->mode == mode);

        if (mode == TrackingMode::kSeparate) {
            iassert(ErrorCodes::InternalError,
                    "Unable to register more than one mutex with separate tracking mode",
                    entries.empty() || !entries.front().token->isValid());
            invariant(entries.size() <= 1);
            entries.clear();
        }
        entries.emplace_back(tag, mode, mutex.token());
#endif
    }

    /**
     * Runs the provided callback on each registered mutex object. The callback is invoked while
     * holding a lock on the registry, so the callback must be tightly-scoped, avoiding allocations
     * and I/O.
     */
    void iterate(CollectionCallback cb) const;

    static ObservableMutexRegistry& get();

private:
    struct MutexEntry {
        StringData tag;
        TrackingMode mode;
        std::shared_ptr<observable_mutex_details::ObservationToken> token;
    };

    mutable stdx::mutex _mutex;
    StringMap<std::list<MutexEntry>> _mutexEntries;
};

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <mutex>

#include <wiredtiger.h>

#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Returns a WT_EVENT_HANDLER with MongoDB's default handlers.
 * The default handlers just log so it is recommended that you consider calling them even if
 * you are capturing the output.
 *
 * There is no default "close" handler. You only need to provide one if you need to call a
 * destructor.
 */
class WiredTigerEventHandler : private WT_EVENT_HANDLER {
public:
    WiredTigerEventHandler();

    WT_EVENT_HANDLER* getWtEventHandler();

    bool wasStartupSuccessful() {
        return _startupSuccessful;
    }

    void setStartupSuccessful() {
        _startupSuccessful = true;
    }

    bool isWtIncompatible() {
        return _wtIncompatible;
    }

    void setWtIncompatible() {
        _wtIncompatible = true;
    }

    /**
     * Updates the current WT connection usable for safe statistics collection, or nullptr if
     * statistics collection is no longer safe.
     *
     * If the WT_CONNECTION is non-null or there are no active statistics readers, the following
     * function returns immediately. Otherwise, if there are active statistics readers and a
     * WT_CONN_CLOSE event is setting this connection to be unavailable for statistics collection,
     * this function waits until all active readers release their collection permits.
     */
    void setWtConnReady(WT_CONNECTION* conn);
    void setWtConnNotReady() {
        setWtConnReady(nullptr);
    }

    /**
     * This function obtains one permit to safely use WiredTiger statistics cursors. When this
     * function returns a non-null connection pointer, the caller may safely collect metrics using
     * this connection, but storage engine shutdown is blocked. The caller *must* make a subsequent
     * call to `releaseStatsCollectionPermit` to allow the storage engine to shut down.
     *
     * If this function returns nullptr, statistics collection is not available due to startup or
     * shutdown.
     */
    WT_CONNECTION* getStatsCollectionPermit();

    /**
     * The following call releases one statistics collection permit. When there are no outstanding
     * permits, the WT connection is allowed to shut down cleanly.
     */
    void releaseStatsCollectionPermit();

    /**
     * Returns the number of outstanding, active statistics collection permits.
     */
    int32_t getActiveStatsReaders() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _activeReaders;
    }

    /**
     * Returns true if the WT connection is ready for statistics cursors to be used. This is unsafe
     * because the connection can become invalid immediately after returning. To ensure the
     * connection stays valid, a permit must be obtained with getStatsCollectionPermit().
     */
    bool isWtConnReadyForStatsCollection() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _wtConn != nullptr;
    }

private:
    bool _startupSuccessful = false;
    bool _wtIncompatible = false;
    mutable std::mutex _mutex;
    WT_CONNECTION* _wtConn = nullptr;
    stdx::condition_variable _idleCondition;
    int32_t _activeReaders{0};
};
}  // namespace mongo

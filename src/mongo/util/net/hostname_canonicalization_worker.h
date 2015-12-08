/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {
class ServiceContext;

/** Provides access to system's FQDNs acquired via forward and reverse resolution of its hostname.
 * A backgrounded worker thread periodically acquires the most current results and stores them
 * in an interal cache.
 */
class HostnameCanonicalizationWorker {
public:
    /** Spawn a new worker thread to acquire FQDNs. */
    HostnameCanonicalizationWorker();

    /** Return a copy of the FQDNs currently in the cache. */
    std::vector<std::string> getCanonicalizedFQDNs() const;

    /** Obtain the HostnameCanonicalizationWorker for a provided ServiceContext. */
    static HostnameCanonicalizationWorker* get(ServiceContext* context);

    /** Spawn the worker thread. */
    static void start(ServiceContext* context);

private:
    // This is the main loop of worker thread, which runs forever.
    void _doWork();

    // Protects _cachedFQDNs
    mutable stdx::mutex _canonicalizationMutex;

    // All FQDNs found in the last pass of the background thread
    std::vector<std::string> _cachedFQDNs;

    // Worker thread
    stdx::thread _canonicalizationThread;
};

}  // namespace mongo

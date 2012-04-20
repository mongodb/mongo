/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <queue>
#include <boost/thread/mutex.hpp>

#include "mongo/db/oplogreader.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace replset {

    class BackgroundSyncInterface {
    public:
        virtual ~BackgroundSyncInterface();
    };


    /**
     * notifierThread() uses lastOpTimeWritten to inform the sync target where this member is
     * currently synced to.
     *
     * Lock order:
     * 1. rslock
     * 2. rwlock
     * 3. BackgroundSync::_mutex
     */
    class BackgroundSync : public BackgroundSyncInterface {
        static BackgroundSync *s_instance;
        // protects creation of s_instance
        static boost::mutex s_mutex;

        // _mutex protects all of the class variables
        boost::mutex _mutex;

        // Tracker thread
        Member* _oplogMarkerTarget;
        OplogReader _oplogMarker; // not locked, only used by notifier thread
        OpTime _consumedOpTime; // not locked, only used by notifier thread

        BackgroundSync();
        BackgroundSync(const BackgroundSync& s);
        BackgroundSync operator=(const BackgroundSync& s);

        // tells the sync target where this member is synced to
        void markOplog();
        bool hasCursor();
    public:
        static BackgroundSync* get();
        virtual ~BackgroundSync() {}

        // starts the sync target notifying thread
        void notifierThread();
    };


} // namespace replset
} // namespace mongo

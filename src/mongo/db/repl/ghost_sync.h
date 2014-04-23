/**
*    Copyright (C) 2008 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/db/repl/member.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/server.h"
#include "mongo/util/concurrency/rwlock.h"

namespace mongo {

    class ReplSetImpl;

    class GhostSync : public task::Server {
        struct GhostSlave : boost::noncopyable {
            GhostSlave() : last(0), slave(0), init(false) { }
            OplogReader reader;
            OpTime last;
            Member* slave;
            bool init;
        };
        /**
         * This is a cache of ghost slaves
         */
        typedef map< mongo::OID,shared_ptr<GhostSlave> > MAP;
        MAP _ghostCache;
        RWLock _lock; // protects _ghostCache
        ReplSetImpl *rs;
        virtual void starting();
    public:
        GhostSync(ReplSetImpl *_rs) : task::Server("rsGhostSync"), _lock("GhostSync"), rs(_rs) {}
        ~GhostSync();

        /**
         * Replica sets can sync in a hierarchical fashion, which throws off w
         * calculation on the master.  percolate() faux-syncs from an upstream
         * node so that the primary will know what the slaves are up to.
         *
         * We can't just directly sync to the primary because it could be
         * unreachable, e.g., S1--->S2--->S3--->P.  S2 should ghost sync from S3
         * and S3 can ghost sync from the primary.
         *
         * Say we have an S1--->S2--->P situation and this node is S2.  rid
         * would refer to S1.  S2 would create a ghost slave of S1 and connect
         * it to P (_currentSyncTarget). Then it would use this connection to
         * pretend to be S1, replicating off of P.
         */
        void percolate(const mongo::OID& rid, const OpTime& last);
        void associateSlave(const BSONObj& rid, const int memberId);
        bool updateSlave(const mongo::OID& id, const OpTime& last);
        void clearCache();
    };
} // namespace mongo

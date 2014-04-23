// /db/repl/rs.h

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

#include "mongo/bson/oid.h"
#include "mongo/bson/optime.h"
#include "mongo/db/commands.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/consensus.h"
#include "mongo/db/repl/heartbeat_info.h"
#include "mongo/db/repl/manager.h"
#include "mongo/db/repl/member.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_set_impl.h"
#include "mongo/db/repl/rs_base.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/db/repl/rs_exception.h"
#include "mongo/db/repl/rs_sync.h"
#include "mongo/db/repl/server.h"
#include "mongo/db/repl/state_box.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/value.h"
#include "mongo/util/net/hostandport.h"

/**
 * Order of Events
 *
 * On startup, if the --replSet option is present, startReplSets is called.
 * startReplSets forks off a new thread for replica set activities.  It creates
 * the global theReplSet variable and calls go() on it.
 *
 * theReplSet's constructor changes the replica set's state to RS_STARTUP,
 * starts the replica set manager, and loads the config (if the replica set
 * has been initialized).
 */

namespace mongo {

    class Cloner;
    class DBClientConnection;
    class GhostSync;
    class ReplSetImpl;
    struct Target;
    extern bool replSet; // true if using repl sets
    extern class ReplSet *theReplSet; // null until initialized
    extern int maxSyncSourceLagSecs;

    class ReplSetCmdline;

    // Main entry point for replica sets
    void startReplSets(ReplSetCmdline *replSetCmdline);

    class ReplSetHealthPollTask;

    class ReplSet : public ReplSetImpl {
    public:
        static ReplSet* make(ReplSetCmdline& replSetCmdline);
        virtual ~ReplSet() {}

        // for the replSetStepDown command
        bool stepDown(int secs) { return _stepDown(secs); }

        // for the replSetFreeze command
        bool freeze(int secs) { return _freeze(secs); }

        string selfFullName() {
            verify( _self );
            return _self->fullName();
        }

        virtual bool buildIndexes() const { return _buildIndexes; }

        /* call after constructing to start - returns fairly quickly after launching its threads */
        void go() { _go(); }
        void shutdown();

        void fatal() { _fatal(); }
        virtual bool isPrimary() { return box.getState().primary(); }
        virtual bool isSecondary() {  return box.getState().secondary(); }
        MemberState state() const { return ReplSetImpl::state(); }
        string name() const { return ReplSetImpl::name(); }
        virtual const ReplSetConfig& config() { return ReplSetImpl::config(); }
        void getOplogDiagsAsHtml(unsigned server_id, stringstream& ss) const { _getOplogDiagsAsHtml(server_id,ss); }
        void summarizeAsHtml(stringstream& ss) const { _summarizeAsHtml(ss); }
        void summarizeStatus(BSONObjBuilder& b) const  { _summarizeStatus(b); }
        void fillIsMaster(BSONObjBuilder& b) { _fillIsMaster(b); }
        threadpool::ThreadPool& getPrefetchPool() { return ReplSetImpl::getPrefetchPool(); }
        threadpool::ThreadPool& getWriterPool() { return ReplSetImpl::getWriterPool(); }

        /**
         * We have a new config (reconfig) - apply it.
         * @param comment write a no-op comment to the oplog about it.  only
         * makes sense if one is primary and initiating the reconf.
         *
         * The slaves are updated when they get a heartbeat indicating the new
         * config.  The comment is a no-op.
         */
        void haveNewConfig(ReplSetConfig& c, bool comment);

        /**
         * Pointer assignment isn't necessarily atomic, so this needs to assure
         * locking, even though we don't delete old configs.
         */
        const ReplSetConfig& getConfig() { return config(); }

        bool lockedByMe() { return RSBase::lockedByMe(); }

        // heartbeat msg to send to others; descriptive diagnostic info
        string hbmsg() const {
            if( time(0)-_hbmsgTime > 120 ) return "";
            return _hbmsg;
        }

    protected:
        ReplSet();
    };

    /**
     * does local authentication
     * directly authorizes against AuthenticationInfo
     */
    void replLocalAuth();

    /** inlines ----------------- */

    inline bool ignoreUniqueIndex(const IndexDescriptor* idx) {
        if (!idx->unique()) {
            return false;
        }
        if (!theReplSet) {
            return false;
        }
        // see SERVER-6671
        MemberState ms = theReplSet->state();
        if (! ((ms == MemberState::RS_STARTUP2) ||
               (ms == MemberState::RS_RECOVERING) ||
               (ms == MemberState::RS_ROLLBACK))) {
            return false;
        }
        // 2 is the oldest oplog version where operations
        // are fully idempotent.
        if (theReplSet->oplogVersion < 2) {
            return false;
        }
        // Never ignore _id index
        if (idx->isIdIndex()) {
            return false;
        }

        return true;
    }

}

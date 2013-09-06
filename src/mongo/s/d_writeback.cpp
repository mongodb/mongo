// d_writeback.cpp

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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/s/d_writeback.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/queue.h"
#include "mongo/util/stacktrace.h"

using namespace std;

namespace mongo {

    // ---------- WriteBackManager class ----------

    // TODO init at mongod startup
    WriteBackManager writeBackManager;

    WriteBackManager::WriteBackManager() : _writebackQueueLock("sharding:writebackQueueLock") {
    }

    WriteBackManager::~WriteBackManager() {
    }

    OID WriteBackManager::queueWriteBack( const string& remote , BSONObjBuilder& b ) {
        static mongo::mutex writebackIDOrdering( "WriteBackManager::queueWriteBack id ordering" );
        
        scoped_lock lk( writebackIDOrdering );

        OID writebackID;
        writebackID.initSequential();
        b.append( "id", writebackID );
        
        getWritebackQueue( remote )->queue.push( b.obj() );

        return writebackID;
    }

    shared_ptr<WriteBackManager::QueueInfo> WriteBackManager::getWritebackQueue( const string& remote ) {
        scoped_lock lk ( _writebackQueueLock );
        shared_ptr<QueueInfo>& q = _writebackQueues[remote];
        if ( ! q )
            q.reset( new QueueInfo() );
        q->lastCall = Listener::getElapsedTimeMillis();
        return q;
    }

    bool WriteBackManager::queuesEmpty() const {
        scoped_lock lk( _writebackQueueLock );
        for ( WriteBackQueuesMap::const_iterator it = _writebackQueues.begin(); it != _writebackQueues.end(); ++it ) {
            const shared_ptr<QueueInfo> queue = it->second;
            if (! queue->queue.empty() ) {
                return false;
            }
        }
        return true;
    }

    void WriteBackManager::appendStats( BSONObjBuilder& b ) const {
        BSONObjBuilder sub;
        long long totalQueued = 0;
        long long now = Listener::getElapsedTimeMillis();
        {
            scoped_lock lk( _writebackQueueLock );
            for ( WriteBackQueuesMap::const_iterator it = _writebackQueues.begin(); it != _writebackQueues.end(); ++it ) {
                const shared_ptr<QueueInfo> queue = it->second;

                BSONObjBuilder t( sub.subobjStart( it->first ) );
                t.appendNumber( "n" , queue->queue.size() );
                t.appendNumber( "minutesSinceLastCall" , ( now - queue->lastCall ) / ( 1000 * 60 ) );
                t.done();

                totalQueued += queue->queue.size();
            }
        }

        b.appendBool( "hasOpsQueued" , totalQueued > 0 );
        b.appendNumber( "totalOpsQueued" , totalQueued );
        b.append( "queues" , sub.obj() );
    }

    bool WriteBackManager::cleanupOldQueues() {
        long long now = Listener::getElapsedTimeMillis();

        scoped_lock lk( _writebackQueueLock );
        for ( WriteBackQueuesMap::iterator it = _writebackQueues.begin(); it != _writebackQueues.end(); ++it ) {
            const shared_ptr<QueueInfo> queue = it->second;
            long long sinceMinutes = ( now - queue->lastCall ) / ( 1000 * 60 );

            if ( sinceMinutes < 60 ) // minutes of inactivity.
                continue;

            log() << "deleting queue from: " << it->first
                  << " of size: " << queue->queue.size()
                  << " after " << sinceMinutes << " inactivity"
                  << " (normal if any mongos has restarted)"
                  << endl;

            _writebackQueues.erase( it );
            return true;
        }
        return false;
    }

    void WriteBackManager::Cleaner::taskDoWork() {
        for ( int i=0; i<1000; i++ ) {
            if ( ! writeBackManager.cleanupOldQueues() )
                break;
        }
    }

    // ---------- admin commands ----------

    // Note, this command will block until there is something to WriteBack
    class WriteBackCommand : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }

        WriteBackCommand() : Command( "writebacklisten" ) {}

        void help(stringstream& h) const { h<<"internal"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::writebacklisten);
            out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
        }
        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

            cc().curop()->suppressFromCurop();
            cc().curop()->setExpectedLatencyMs( 30000 );

            BSONElement e = cmdObj.firstElement();
            if ( e.type() != jstOID ) {
                errmsg = "need oid as first value";
                return 0;
            }

            // get the command issuer's (a mongos) serverID
            const OID id = e.__oid();

            // the command issuer is blocked awaiting a response
            // we want to do return at least at every 5 minutes so sockets don't timeout
            BSONObj z;
            if ( writeBackManager.getWritebackQueue(id.str())->queue.blockingPop( z, 5 * 60 /* 5 minutes */ ) ) {
                LOG(1) << "WriteBackCommand got : " << z << endl;
                result.append( "data" , z );
            }
            else {
                result.appendBool( "noop" , true );
            }

#ifdef _DEBUG
            PseudoRandom r(static_cast<int64_t>(time(0)));
            // Sleep a short amount of time usually
            int sleepFor = r.nextInt32( 10 );
            sleepmillis( sleepFor );

            // Sleep a longer amount of time every once and awhile
            int sleepLong = r.nextInt32( 50 );
            if( sleepLong == 0 ) sleepsecs( 2 );
#endif

            return true;
        }
    } writeBackCommand;

    class WriteBacksQueuedCommand : public Command {
    public:
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::writeBacksQueued);
            out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
        }
        WriteBacksQueuedCommand() : Command( "writeBacksQueued" ) {}

        void help(stringstream& help) const {
            help << "Returns whether there are operations in the writeback queue at the time the command was called. "
                 << "This is an internal command";
        }

        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            writeBackManager.appendStats( result );
            return true;
        }

    } writeBacksQueuedCommand;

    class WriteBacksQueuedSSM : public ServerStatusMetric {
    public:
        WriteBacksQueuedSSM() : ServerStatusMetric(".writeBacksQueued"){}
        virtual void appendAtLeaf( BSONObjBuilder& b ) const {
            b.appendBool( _leafName, ! writeBackManager.queuesEmpty() );
        }
    } writeBacksQueuedSSM;

}  // namespace mongo

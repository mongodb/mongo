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
*/

#include "pch.h"

#include "../db/commands.h"
#include "../util/queue.h"
#include "../util/net/listen.h"
#include "../db/curop.h"
#include "../db/client.h"

#include "d_writeback.h"

using namespace std;

namespace mongo {

    // ---------- WriteBackManager class ----------

    // TODO init at mongod startup
    WriteBackManager writeBackManager;

    WriteBackManager::WriteBackManager() : _writebackQueueLock("sharding:writebackQueueLock") {
    }

    WriteBackManager::~WriteBackManager() {
    }

    void WriteBackManager::queueWriteBack( const string& remote , const BSONObj& o ) {
        static mongo::mutex xxx( "WriteBackManager::queueWriteBack tmp" );
        static OID lastOID;

        scoped_lock lk( xxx );
        const BSONElement& e = o["id"];
        
        if ( lastOID.isSet() ) {
            if ( e.OID() < lastOID ) {
                log() << "this could fail" << endl;
                printStackTrace();
            }
        }
        lastOID = e.OID();
        getWritebackQueue( remote )->queue.push( o );
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
            // Sleep a short amount of time usually
            int sleepFor = rand() % 10;
            sleepmillis( sleepFor );

            // Sleep a longer amount of time every once and awhile
            int sleepLong = rand() % 50;
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
    

}  // namespace mongo

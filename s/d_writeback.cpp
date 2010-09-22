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
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"

#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"

using namespace std;

namespace mongo {
    // a map from mongos's serverIDs to queues of "rejected" operations
    // an operation is rejected if it targets data that does not live on this shard anymore
    typedef map< string , BlockingQueue<BSONObj>* > WriteBackQueuesMap;

    // 'writebackQueueLock' protects only the map itself, since each queue is syncrhonized.
    static mongo::mutex writebackQueueLock("sharding:writebackQueueLock");
    static WriteBackQueuesMap writebackQueues;

    BlockingQueue<BSONObj>* getWritebackQueue( const string& remote ){
        scoped_lock lk (writebackQueueLock );
        BlockingQueue<BSONObj>*& q = writebackQueues[remote];
        if ( ! q )
            q = new BlockingQueue<BSONObj>();
        return q;
    }
    
    void queueWriteBack( const string& remote , const BSONObj& o ){
        getWritebackQueue( remote )->push( o );
    }

    // Note, this command will block until there is something to WriteBack
    class WriteBackCommand : public Command {
    public:
        virtual LockType locktype() const { return NONE; } 
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        
        WriteBackCommand() : Command( "writebacklisten" ){}

        void help(stringstream& h) const { h<<"internal"; }

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){

            BSONElement e = cmdObj.firstElement();
            if ( e.type() != jstOID ){
                errmsg = "need oid as first value";
                return 0;
            }

            // get the command issuer's (a mongos) serverID
            const OID id = e.__oid();
            
            // the command issuer is blocked awaiting a response
            // we want to do return at least at every 5 minutes so sockets don't timeout
            BSONObj z;
            if ( getWritebackQueue(id.str())->blockingPop( z, 5 * 60 /* 5 minutes */ ) ) {
                log(1) << "WriteBackCommand got : " << z << endl;
                result.append( "data" , z );
            }
            else {
                result.appendBool( "noop" , true );
            }
            
            return true;
        }
    } writeBackCommand;

    class WriteBacksQueuedCommand : public Command {
    public:
        virtual LockType locktype() const { return NONE; } 
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        
        WriteBacksQueuedCommand() : Command( "writeBackQueued" ){}

        void help(stringstream& help) const { 
            help << "Returns whether there are operations in the writeback queue at the time the command was called. "
                 << "This is an internal comand"; 
        }

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            bool hasOpsQueued = false;
            {
                scoped_lock lk(writebackQueueLock );
                for ( WriteBackQueuesMap::const_iterator it = writebackQueues.begin(); it != writebackQueues.end(); ++it ){
                    const BlockingQueue<BSONObj>* queue = it->second;
                    if (! queue->empty() ){
                        hasOpsQueued = true;
                        break;
                    }
                }
            }

            result.appendBool( "hasOpsQueued" , hasOpsQueued );
            return true;
        }

    } writeBacksQueuedCommand;

}  // namespace mongo

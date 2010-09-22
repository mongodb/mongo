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

#include "d_writeback.h"

using namespace std;

namespace mongo {

    // ---------- WriteBackManager class ----------

    // TODO init at mongod startup
    WriteBackManager writeBackManager;

    WriteBackManager::WriteBackManager() : _writebackQueueLock("sharding:writebackQueueLock"){
    }

    WriteBackManager::~WriteBackManager(){
    }

    void WriteBackManager::queueWriteBack( const string& remote , const BSONObj& o ){
        getWritebackQueue( remote )->push( o );
    }

    BlockingQueue<BSONObj>* WriteBackManager::getWritebackQueue( const string& remote ){
        scoped_lock lk ( _writebackQueueLock );
        BlockingQueue<BSONObj>*& q = _writebackQueues[remote];
        if ( ! q )
            q = new BlockingQueue<BSONObj>();
        return q;
    }    

    bool WriteBackManager::queuesEmpty() const{
        scoped_lock lk( _writebackQueueLock );
        for ( WriteBackQueuesMap::const_iterator it = _writebackQueues.begin(); it != _writebackQueues.end(); ++it ){
            const BlockingQueue<BSONObj>* queue = it->second;
            if (! queue->empty() ){
                return false;
            }
        }
        return true;
    }

    // ---------- admin commands ----------

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
            if ( writeBackManager.getWritebackQueue(id.str())->blockingPop( z, 5 * 60 /* 5 minutes */ ) ) {
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
        
        WriteBacksQueuedCommand() : Command( "writeBacksQueued" ){}

        void help(stringstream& help) const { 
            help << "Returns whether there are operations in the writeback queue at the time the command was called. "
                 << "This is an internal comand"; 
        }

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            result.appendBool( "hasOpsQueued" , ! writeBackManager.queuesEmpty() );
            return true;
        }

    } writeBacksQueuedCommand;

}  // namespace mongo

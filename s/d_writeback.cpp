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

    map< string , BlockingQueue<BSONObj>* > writebackQueue;
    mongo::mutex writebackQueueLock("sharding:writebackQueueLock");

    BlockingQueue<BSONObj>* getWritebackQueue( const string& remote ){
        scoped_lock lk (writebackQueueLock );
        BlockingQueue<BSONObj>*& q = writebackQueue[remote];
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
            
            const OID id = e.__oid();
            
            try {
                // we want to do something every 5 minutes so sockets don't timeout
                BSONObj z = getWritebackQueue(id.str())->blockingPop( 5 * 60 /* 5 minutes */ );
                log(1) << "WriteBackCommand got : " << z << endl;
                result.append( "data" , z );
            }
            catch ( BlockingQueue<BSONObj>::Timeout& t ){
                result.appendBool( "noop" , true );
            }
            
            return true;
        }
    } writeBackCommand;

}

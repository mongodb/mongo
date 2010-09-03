// d_logic.cpp

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


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "pch.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"

#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"

using namespace std;

namespace mongo {

    bool handlePossibleShardedMessage( Message &m, DbResponse* dbresponse ){
        if ( ! shardingState.enabled() )
            return false;

        int op = m.operation();
        if ( op < 2000 
             || op >= 3000 
             || op == dbGetMore  // cursors are weird
             )
            return false;
        
        DbMessage d(m);        
        const char *ns = d.getns();
        string errmsg;
        if ( shardVersionOk( ns , opIsWrite( op ) , errmsg ) ){
            return false;
        }

        log() << "shardVersionOk failed  ns:(" << ns << ") op:(" << opToString(op) << ") " << errmsg << endl;
        
        if ( doesOpGetAResponse( op ) ){
            assert( dbresponse );
            BufBuilder b( 32768 );
            b.skip( sizeof( QueryResult ) );
            {
                BSONObj obj = BSON( "$err" << errmsg );
                b.appendBuf( obj.objdata() , obj.objsize() );
            }
            
            QueryResult *qr = (QueryResult*)b.buf();
            qr->_resultFlags() = ResultFlag_ErrSet | ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation( opReply );
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            b.decouple();

            Message * resp = new Message();
            resp->setData( qr , true );
            
            dbresponse->response = resp;
            dbresponse->responseTo = m.header()->id;
            return true;
        }
        
        OID writebackID;
        writebackID.init();
        lastError.getSafe()->writeback( writebackID );

        const OID& clientID = ShardedConnectionInfo::get(false)->getID();
        massert( 10422 ,  "write with bad shard config and no server id!" , clientID.isSet() );
        
        log(1) << "got write with an old config - writing back ns: " << ns << endl;
        if ( logLevel ) log(1) << debugString( m ) << endl;

        BSONObjBuilder b;
        b.appendBool( "writeBack" , true );
        b.append( "ns" , ns );
        b.append( "id" , writebackID );
        b.appendTimestamp( "version" , shardingState.getVersion( ns ) );
        b.appendTimestamp( "yourVersion" , ShardedConnectionInfo::get( true )->getVersion( ns ) );
        b.appendBinData( "msg" , m.header()->len , bdtCustom , (char*)(m.singleData()) );
        log(2) << "writing back msg with len: " << m.header()->len << " op: " << m.operation() << endl;
        queueWriteBack( clientID.str() , b.obj() );

        return true;
    }

}

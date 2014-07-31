// @file d_logic.cpp

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


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/d_logic.h"

#include <string>

#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/shard.h"
#include "mongo/util/log.h"

using namespace std;

namespace mongo {

    bool _checkShardVersion(Message &m, DbResponse* dbresponse) {
        DEV verify( shardingState.enabled() );

        int op = m.operation();
        if ( op < 2000
                || op >= 3000
                || op == dbGetMore  // cursors are weird
           )
            return true;

        DbMessage d(m);
        const char *ns = d.getns();
        string errmsg;
        ChunkVersion received, wanted;
        if ( shardVersionOk( ns , errmsg, received, wanted ) ) {
            return true;
        }

        LOG(1) << "connection sharding metadata does not match for collection " << ns
               << ", will retry (wanted : " << wanted
               << ", received : " << received << ")" << endl;

        fassert(18664, op == dbQuery || op == dbGetMore);

        verify( dbresponse );
        BufBuilder b( 32768 );
        b.skip( sizeof( QueryResult::Value ) );
        {
            BSONObjBuilder bob;

            bob.append( "$err", errmsg );
            bob.append( "ns", ns );
            wanted.addToBSON( bob, "vWanted" );
            received.addToBSON( bob, "vReceived" );

            BSONObj obj = bob.obj();

            b.appendBuf( obj.objdata() , obj.objsize() );
        }

        QueryResult::View qr = b.buf();
        qr.setResultFlags(ResultFlag_ErrSet | ResultFlag_ShardConfigStale);
        qr.msgdata().setLen(b.len());
        qr.msgdata().setOperation( opReply );
        qr.setCursorId(0);
        qr.setStartingFrom(0);
        qr.setNReturned(1);
        b.decouple();

        Message * resp = new Message();
        resp->setData(qr.view2ptr(), true);

        dbresponse->response = resp;
        dbresponse->responseTo = m.header().getId();
        return false;
    }

}

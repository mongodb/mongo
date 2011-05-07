// introspect.cpp

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
#include "introspect.h"
#include "../bson/util/builder.h"
#include "../util/goodies.h"
#include "pdfile.h"
#include "jsobj.h"
#include "pdfile.h"
#include "curop.h"

namespace mongo {

    BufBuilder profileBufBuilder; // reused, instead of allocated every time - avoids a malloc/free cycle

    void profile( const Client& c , CurOp& currentOp, int millis) {
        assertInWriteLock();
        
        profileBufBuilder.reset();
        BSONObjBuilder b(profileBufBuilder);
        b.appendDate("ts", jsTime());
        const string info = currentOp.debug().toString();
        b.append("info", info);
        b.append("millis", (double) millis);
        if ( currentOp.getNS() )
            b.append( "ns" , currentOp.getNS() );
        b.append("client", c.clientAddress() );

        BSONObj p = b.done();

        // write: not replicated
        Database *db = c.database();
        const char *ns = db->profileName.c_str();
        NamespaceDetails *d = db->namespaceIndex.details(ns);
        if( d ) {
            int len = p.objsize();
            Record *r = theDataFileMgr.fast_oplog_insert(d, ns, len);
            memcpy(getDur().writingPtr(r->data, len), p.objdata(), len);
        }
        else { 
            static time_t last;
            if( time(0) > last+10 ) {
                log() << "profile: warning ns " << ns << " does not exist" << endl;
                last = time(0);
            }
        }
    }

} // namespace mongo

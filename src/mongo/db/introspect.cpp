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

    void profile( const Client& c , CurOp& currentOp ) {
        verify( Lock::somethingWriteLocked() );

        Database *db = c.database();
        DEV verify( db );
        const char *ns = db->profileName.c_str();
        
        // build object
        profileBufBuilder.reset();
        BSONObjBuilder b(profileBufBuilder);
        b.appendDate("ts", jsTime());
        currentOp.debug().append( currentOp , b );

        b.append("client", c.clientAddress() );

        if ( c.getAuthenticationInfo() )
            b.append( "user" , c.getAuthenticationInfo()->getUser( nsToDatabase( ns ) ) );

        BSONObj p = b.done();

        if (p.objsize() > 100*1024){
            string small = p.toString(/*isArray*/false, /*full*/false);

            warning() << "can't add full line to system.profile: " << small;

            // rebuild with limited info
            BSONObjBuilder b(profileBufBuilder);
            b.appendDate("ts", jsTime());
            b.append("client", c.clientAddress() );
            if ( c.getAuthenticationInfo() )
                b.append( "user" , c.getAuthenticationInfo()->getUser( nsToDatabase( ns ) ) );

            b.append("err", "profile line too large (max is 100KB)");
            if (small.size() < 100*1024){ // should be much smaller but if not don't break anything
                b.append("abbreviated", small);
            }

            p = b.done();
        }

        // write: not replicated
        NamespaceDetails *d = db->namespaceIndex.details(ns);
        if( d ) {
            int len = p.objsize();
            Record *r = theDataFileMgr.fast_oplog_insert(d, ns, len);
            memcpy(getDur().writingPtr(r->data(), len), p.objdata(), len);
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

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

    void profile( const Client& c , CurOp& currentOp, int millis) {
        assertInWriteLock();
        
        string info = currentOp.debug().str.str();
        int initSize = info.size() + 64;

        BSONObjBuilder b( initSize );

        b.appendDate("ts", jsTime());
        b.append("info", info);
        b.append("millis", (double) millis);
        if ( currentOp.getNS() )
            b.append( "ns" , currentOp.getNS() );
        b.append("client", c.clientAddress() );


        BSONObj p = b.done();
        
        if ( p.objsize() > initSize ) {
            RARELY warning() << "profile had to increase size of BSONObj : " << p << endl;
        }

        theDataFileMgr.insert(c.database()->profileName.c_str(), p.objdata(), p.objsize(), true);
    }

} // namespace mongo

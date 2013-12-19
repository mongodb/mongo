// insert.cpp

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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/ops/insert.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    StatusWith<BSONObj> fixDocumentForInsert( const BSONObj& doc ) {
        if ( doc.objsize() > BSONObjMaxUserSize )
            return StatusWith<BSONObj>( ErrorCodes::BadValue, "object to insert too large" );

        bool firstElementIsId = doc.firstElement().fieldNameStringData() == "_id";
        bool hasTimestampToFix = false;
        {
            BSONObjIterator i( doc );
            while ( i.more() ) {
                BSONElement e = i.next();

                if ( e.type() == Timestamp && e.timestampValue() == 0 ) {
                    hasTimestampToFix = true;
                    break;
                }

                const char* fieldName = e.fieldName();

                if ( fieldName[0] == '$' ) {
                    return StatusWith<BSONObj>( ErrorCodes::BadValue,
                                                str::stream()
                                                << "Document can't have $ prefixed field names: "
                                                << e.fieldName() );
                }

                // check no regexp for _id (SERVER-9502)
                // also, disallow undefined and arrays
                if ( str::equals( fieldName, "_id") ) {
                    if ( e.type() == RegEx ) {
                        return StatusWith<BSONObj>( ErrorCodes::BadValue,
                                                    "can't use a regex for _id" );
                    }
                    if ( e.type() == Undefined ) {
                        return StatusWith<BSONObj>( ErrorCodes::BadValue,
                                                    "can't use a undefined for _id" );
                    }
                    if ( e.type() == Array ) {
                        return StatusWith<BSONObj>( ErrorCodes::BadValue,
                                                    "can't use a array for _id" );
                    }
                }

            }
        }

        if ( firstElementIsId && !hasTimestampToFix )
            return StatusWith<BSONObj>( BSONObj() );

        bool hadId = firstElementIsId;

        BSONObjIterator i( doc );
        int pos = 0;

        BSONObjBuilder b( doc.objsize() + 16 );
        if ( firstElementIsId ) {
            b.append( doc.firstElement() );
            i.next();
            pos++;
        }
        else {
            BSONElement e = doc["_id"];
            if ( e.type() ) {
                b.append( e );
                hadId = true;
            }
            else {
                b.appendOID( "_id", NULL, true );
            }
        }

        while ( i.more() ) {
            BSONElement e = i.next();
            if ( hadId && e.fieldNameStringData() == "_id" ) {
                // no-op
            }
            else if ( e.type() == Timestamp && e.timestampValue() == 0 ) {
                mutex::scoped_lock lk(OpTime::m);
                b.append( e.fieldName(), OpTime::now(lk) );
            }
            else {
                b.append( e );
            }
            pos++;
        }
        return StatusWith<BSONObj>( b.obj() );
    }

}

/** @file docgenerator.cpp

*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/tools/docgenerator.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    void DocumentGenerator::init( BSONObj& args ) {
        uassert( 16363, "_id is not a number", args["_id"].isNumber() );
        config.id = args["_id"].numberLong();

        uassert( 16364, "blob is not a string", (args["blob"].type() == String) );
        config.blob = args["blob"].String();

        uassert( 16365, "nestedDoc is not an object", (args["nestedDoc"].type() == Object) );
        config.nestedDoc = args["nestedDoc"].embeddedObject();

        uassert( 16366, "list is not an array", args["list"].type() == Array );
        BSONObj list = args["list"].embeddedObject();
        for( int i = 0; i < 10; i++ ) {
            uassert( 16367, "list member is not a string", list[i].type() == String );
            config.list.push_back( list[i].String() );
        }

        uassert( 16368, "counter is not a number", args["counter"].isNumber() );
        config.counter = args["counter"].numberLong();
    }


    BSONObj DocumentGenerator::createDocument() {
        BSONObjBuilder doc;
        doc.append( "_id", config.id );
        config.id++;
        doc.append( "blob", config.blob );
        doc.append( "nestedDoc", config.nestedDoc );
        doc.append( "list", config.list );
        doc.append( "counter", config.counter );
        config.counter++;
        return doc.obj();
    }
}


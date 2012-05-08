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
        uassert( 16261, "blob is not a string", (args["blob"].type() == String) );
        config.blob = args["blob"].String();

        uassert( 16262, "md5 seed is not a string", (args["md5seed"].type() == String) );
        config.md5seed = args["md5seed"].String();

        uassert( 16263, "counterUp is not a number", args["counterUp"].isNumber() );
        config.counterUp = args["counterUp"].numberLong();

        uassert( 16264, "counterDown is not a number", args["counterDown"].isNumber() );
        config.counterDown = args["counterDown"].numberLong();
    }


    BSONObj DocumentGenerator::createDocument() {
        BSONObjBuilder doc;
        doc.genOID();

        doc.append( "counterUp" , config.counterUp );
        string hashUp = md5simpledigest( mongoutils::str::stream() << config.md5seed <<  config.counterUp );
        hashUp = hashUp.substr( 0, 8 );
        doc.append( "hashIdUp", atoll(hashUp.c_str()) );
        config.counterUp++;

        doc.append( "blobData" , config.blob );

        doc.append( "counterDown" , config.counterDown );
        string hashDown = md5simpledigest(  mongoutils::str::stream() << config.md5seed <<  config.counterDown );
        hashDown = hashDown.substr( 0, 16 );
        doc.append( "hashIdDown", atoll(hashDown.c_str()) );

        config.counterDown--;

        return doc.obj();
    }
}


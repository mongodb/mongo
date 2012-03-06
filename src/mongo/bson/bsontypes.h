// bsontypes.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "util/misc.h"

namespace bson { }

namespace mongo {

    class BSONArrayBuilder;
    class BSONElement;
    class BSONObj;
    class BSONObjBuilder;
    class BSONObjBuilderValueStream;
    class BSONObjIterator;
    class Ordering;
    class Record;
    struct BSONArray; // empty subclass of BSONObj useful for overloading
    struct BSONElementCmpWithoutField;

    extern BSONObj maxKey;
    extern BSONObj minKey;

    /**
        the complete list of valid BSON types
        see also bsonspec.org
    */
    enum BSONType {
        /** smaller than all other types */
        MinKey=-1,
        /** end of object */
        EOO=0,
        /** double precision floating point value */
        NumberDouble=1,
        /** character string, stored in utf8 */
        String=2,
        /** an embedded object */
        Object=3,
        /** an embedded array */
        Array=4,
        /** binary data */
        BinData=5,
        /** Undefined type */
        Undefined=6,
        /** ObjectId */
        jstOID=7,
        /** boolean type */
        Bool=8,
        /** date type */
        Date=9,
        /** null type */
        jstNULL=10,
        /** regular expression, a pattern with options */
        RegEx=11,
        /** deprecated / will be redesigned */
        DBRef=12,
        /** deprecated / use CodeWScope */
        Code=13,
        /** a programming language (e.g., Python) symbol */
        Symbol=14,
        /** javascript code that can execute on the database server, with SavedContext */
        CodeWScope=15,
        /** 32 bit signed integer */
        NumberInt = 16,
        /** Updated to a Date with value next OpTime on insert */
        Timestamp = 17,
        /** 64 bit integer */
        NumberLong = 18,
        /** max type that is not MaxKey */
        JSTypeMax=18,
        /** larger than all other types */
        MaxKey=127
    };

    /* subtypes of BinData.
       bdtCustom and above are ones that the JS compiler understands, but are
       opaque to the database.
    */
    enum BinDataType {
        BinDataGeneral=0,
        Function=1,
        ByteArrayDeprecated=2, /* use BinGeneral instead */
        bdtUUID = 3, /* deprecated */
	newUUID=4, /* language-independent UUID format across all drivers */
        MD5Type=5,
        bdtCustom=128
    };

}


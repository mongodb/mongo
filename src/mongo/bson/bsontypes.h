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

#include "mongo/util/assert_util.h"

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

    /**
     * returns the name of the argument's type
     * defined in jsobj.cpp
     */
    const char* typeName (BSONType type);

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

    /** Returns a number for where a given type falls in the sort order.
     *  Elements with the same return value should be compared for value equality.
     *  The return value is not a BSONType and should not be treated as one.
     *  Note: if the order changes, indexes have to be re-built or than can be corruption
     */
    inline int canonicalizeBSONType(BSONType type) {
        switch (type) {
        case MinKey:
        case MaxKey:
            return type;
        case EOO:
        case Undefined:
            return 0;
        case jstNULL:
            return 5;
        case NumberDouble:
        case NumberInt:
        case NumberLong:
            return 10;
        case mongo::String:
        case Symbol:
            return 15;
        case Object:
            return 20;
        case mongo::Array:
            return 25;
        case BinData:
            return 30;
        case jstOID:
            return 35;
        case mongo::Bool:
            return 40;
        case mongo::Date:
        case Timestamp:
            return 45;
        case RegEx:
            return 50;
        case DBRef:
            return 55;
        case Code:
            return 60;
        case CodeWScope:
            return 65;
        default:
            verify(0);
            return -1;
        }
    }
}

/** @file bson_db.h

    This file contains the implementation of BSON-related methods that are required
    by the MongoDB database server.

    Normally, for standalone BSON usage, you do not want this file - it will tend to
    pull in some other files from the MongoDB project. Thus, bson.h (the main file
    one would use) does not include this file.
*/

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

#include "../util/optime.h"
#include "../util/time_support.h"

namespace mongo {

    /**
    Timestamps are a special BSON datatype that is used internally for replication.
    Append a timestamp element to the object being ebuilt.
    @param time - in millis (but stored in seconds)
    */
    inline BSONObjBuilder& BSONObjBuilder::appendTimestamp( const StringData& fieldName , unsigned long long time , unsigned int inc ) {
        OpTime t( (unsigned) (time / 1000) , inc );
        appendTimestamp( fieldName , t.asDate() );
        return *this;
    }

    inline OpTime BSONElement::_opTime() const {
        if( type() == mongo::Date || type() == Timestamp )
            return OpTime( little<unsigned long long>::ref( value() ) );
        return OpTime();
    }

    inline std::string BSONElement::_asCode() const {
        switch( type() ) {
        case mongo::String:
        case Code:
            return std::string(valuestr(), valuestrsize()-1);
        case CodeWScope:
            return std::string(codeWScopeCode(), little<int>::ref(valuestr()) - 1 );
        default:
            log() << "can't convert type: " << (int)(type()) << " to code" << std::endl;
        }
        uassert( 10062 ,  "not code" , 0 );
        return "";
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(DateNowLabeler& id) {
        _builder->appendDate(_fieldName, jsTime());
        _fieldName = 0;
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(NullLabeler& id) {
        _builder->appendNull(_fieldName);
        _fieldName = 0;
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(MinKeyLabeler& id) {
        _builder->appendMinKey(_fieldName);
        _fieldName = 0;
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(MaxKeyLabeler& id) {
        _builder->appendMaxKey(_fieldName);
        _fieldName = 0;
        return *_builder;
    }

}

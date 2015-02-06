/** @file bson_db.h */

/*    Copyright 2009 10gen Inc.
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

/*
    This file contains the implementation of BSON-related methods that are required
    by the MongoDB database server.

    Normally, for standalone BSON usage, you do not want this file - it will tend to
    pull in some other files from the MongoDB project. Thus, bson.h (the main file
    one would use) does not include this file.
*/

#pragma once

#include "mongo/base/data_view.h"
#include "mongo/bson/optime.h"
#include "mongo/util/time_support.h"

namespace mongo {

    /**
    Timestamps are a special BSON datatype that is used internally for replication.
    Append a timestamp element to the object being ebuilt.
    @param time - in millis (but stored in seconds)
    */
    inline BSONObjBuilder& BSONObjBuilder::appendTimestamp( StringData fieldName , unsigned long long time , unsigned int inc ) {
        OpTime t( (unsigned) (time / 1000) , inc );
        appendTimestamp( fieldName , t.asDate() );
        return *this;
    }

    inline BSONObjBuilder& BSONObjBuilder::append(StringData fieldName, OpTime optime) {
        appendTimestamp(fieldName, optime.asDate());
        return *this;
    }

    inline OpTime BSONElement::_opTime() const {
        if( type() == mongo::Date || type() == Timestamp )
            return OpTime(ConstDataView(value()).readLE<unsigned long long>());
        return OpTime();
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const DateNowLabeler& id) {
        _builder->appendDate(_fieldName, jsTime());
        _fieldName = StringData();
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const NullLabeler& id) {
        _builder->appendNull(_fieldName);
        _fieldName = StringData();
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const UndefinedLabeler& id) {
        _builder->appendUndefined(_fieldName);
        _fieldName = StringData();
        return *_builder;
    }


    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const MinKeyLabeler& id) {
        _builder->appendMinKey(_fieldName);
        _fieldName = StringData();
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const MaxKeyLabeler& id) {
        _builder->appendMaxKey(_fieldName);
        _fieldName = StringData();
        return *_builder;
    }

    template<class T> inline
    BSONObjBuilder& BSONObjBuilderValueStream::operator<<( T value ) {
        _builder->append(_fieldName, value);
        _fieldName = StringData();
        return *_builder;
    }

    template<class T>
    BSONObjBuilder& Labeler::operator<<( T value ) {
        s_->subobj()->append( l_.l_, value );
        return *s_->_builder;
    }

}

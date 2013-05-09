// matchable.h

/**
 *    Copyright (C) 2013 10gen Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"

namespace mongo {

    class MatchableDocument {
    public:
        virtual ~MatchableDocument();

        virtual BSONObj toBSON() const = 0;

        virtual BSONElement getFieldDottedOrArray( const FieldRef& path,
                                                   size_t* idxPath,
                                                   bool* inArray ) const = 0;

        virtual void getFieldsDotted( const StringData& name,
                                      BSONElementSet &ret,
                                      bool expandLastArray = true ) const = 0;

    };

    class BSONMatchableDocument : public MatchableDocument {
    public:
        BSONMatchableDocument( const BSONObj& obj );
        virtual ~BSONMatchableDocument();

        virtual BSONObj toBSON() const { return _obj; }

        virtual BSONElement getFieldDottedOrArray( const FieldRef& path,
                                                   size_t* idxPath,
                                                   bool* inArray ) const;

        virtual void getFieldsDotted( const StringData& name,
                                      BSONElementSet &ret,
                                      bool expandLastArray = true ) const;

    private:
        BSONObj _obj;
    };
}

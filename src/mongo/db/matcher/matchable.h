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
#include "mongo/db/matcher/path.h"

namespace mongo {

    class MatchableDocument {
    public:
        virtual ~MatchableDocument();

        virtual BSONObj toBSON() const = 0;

        virtual ElementIterator* getIterator( const ElementPath& path ) const = 0;

    };

    class BSONMatchableDocument : public MatchableDocument {
    public:
        BSONMatchableDocument( const BSONObj& obj );
        virtual ~BSONMatchableDocument();

        virtual BSONObj toBSON() const { return _obj; }

        virtual ElementIterator* getIterator( const ElementPath& path ) const {
            return new BSONElementIterator( path, _obj );
        }

    private:
        BSONObj _obj;
    };
}

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

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_internal.h"
#include "mongo/db/matcher/matchable.h"

namespace mongo {

    MatchableDocument::~MatchableDocument(){
    }

    BSONMatchableDocument::BSONMatchableDocument( const BSONObj& obj )
        : _obj( obj ) {
    }

    BSONMatchableDocument::~BSONMatchableDocument() {
    }


    BSONElement BSONMatchableDocument::getFieldDottedOrArray( const FieldRef& path,
                                                              int32_t* idxPath,
                                                              bool* inArray ) const {
        return mongo::getFieldDottedOrArray( _obj, path, idxPath, inArray );
    }

    void BSONMatchableDocument::getFieldsDotted( const StringData& name,
                                                 BSONElementSet &ret,
                                                 bool expandLastArray ) const {
        return _obj.getFieldsDotted( name, ret, expandLastArray );
    }

}

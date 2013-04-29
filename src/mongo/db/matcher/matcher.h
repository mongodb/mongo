// matcher.h

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
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/match_details.h"

namespace mongo {

    class Matcher2 : boost::noncopyable {
    public:
        Matcher2( const BSONObj& pattern );

        bool matches(const BSONObj& doc, MatchDetails* details = NULL ) const;

    private:
        const BSONObj& _pattern; // this is owned by who created us
        boost::scoped_ptr<Expression> _expression;
    };

}

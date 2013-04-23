// expression.cpp

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

#include "mongo/db/matcher/expression.h"

#include "mongo/bson/bsonobjiterator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/matcher.h"
#include "mongo/util/log.h"

namespace mongo {

    string Expression::toString() const {
        StringBuilder buf;
        debugString( buf, 0 );
        return buf.str();
    }

    void Expression::_debugAddSpace( StringBuilder& debug, int level ) const {
        for ( int i = 0; i < level; i++ )
            debug << "    ";
    }

}



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

#include "mongo/db/index_names.h"

#include "mongo/db/jsobj.h"

namespace mongo {
    const string IndexNames::GEO_2D = "2d";
    const string IndexNames::GEO_HAYSTACK = "geoHaystack";
    const string IndexNames::GEO_2DSPHERE = "2dsphere";
    const string IndexNames::TEXT = "text";
    const string IndexNames::TEXT_INTERNAL = "_fts";
    const string IndexNames::HASHED = "hashed";

    // static
    string IndexNames::findPluginName(const BSONObj& keyPattern) {
        BSONObjIterator i(keyPattern);

        while (i.more()) {
            BSONElement e = i.next();
            if (String != e.type()) { continue; }
            return e.String();
        }

        return "";
    }

}  // namespace mongo

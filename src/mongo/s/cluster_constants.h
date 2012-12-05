/**
 *    Copyright (C) 2012 10gen Inc.
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

#include <string>

#include "mongo/bson/util/misc.h"  // for Date_t
#include "mongo/db/jsobj.h"        // for BSON_Field and dependencies

namespace mongo {

    using std::string;

    /**
     * ConfigNS holds the names for all the metadata collections stored in a config server.
     */
    struct ConfigNS {
        static const string locks;

        static const int version = 3;
    };

} // namespace mongo

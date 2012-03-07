// mongo/db/authlevel.h

/**
 *    Copyright (C) 2009 10gen Inc.
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

namespace mongo {

    /*
     * for a particular db
     * levels
     *     0 : none
     *     1 : read
     *     2 : write
     */
    struct Auth {

        enum Level { NONE = 0 ,
                     READ = 1 ,
                     WRITE = 2 };

        Auth() : level( NONE ) {}

        Level level;
        string user;
    };
}  // namespace mongo

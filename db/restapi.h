/** @file restapi.h
 */

/**
*    Copyright (C) 2010 10gen Inc.
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

#include "../util/admin_access.h"

namespace mongo {

    class RestAdminAccess : public AdminAccess {
    public:
        virtual ~RestAdminAccess() { }

        virtual bool haveAdminUsers() const;
        virtual BSONObj getAdminUser( const string& username ) const;
    };

} // namespace mongo

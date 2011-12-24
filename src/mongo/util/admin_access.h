/** @file admin_access.h
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

namespace mongo {

    /*
     * An AdminAccess is an interface class used to determine if certain users have
     * priviledges to a given resource.
     *
     */
    class AdminAccess {
    public:
        virtual ~AdminAccess() { }

        /** @return if there are any priviledge users. This should not
         *          block for long and throw if can't get a lock if needed.
         */
        virtual bool haveAdminUsers() const = 0;

        /** @return priviledged user with this name. This should not block
         *          for long and throw if can't get a lock if needed
         */
        virtual BSONObj getAdminUser( const string& username ) const = 0;
    };

    class NoAdminAccess : public AdminAccess {
    public:
        virtual ~NoAdminAccess() { }

        virtual bool haveAdminUsers() const { return false; }
        virtual BSONObj getAdminUser( const string& username ) const { return BSONObj(); }
    };

}  // namespace mongo

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

#include "mongo/client/dbclientinterface.h"

namespace mongo {
    /**
     * @return true if internal authentication parameters has been set up
     */
    extern bool isInternalAuthSet();

    /**
     * This method initializes the internalSecurity object with authentication
     * credentials to be used by authenticateInternalUser. This method should 
     * only be called once when setting up authentication method for the system.
     */
    extern bool setInternalUserAuthParams(BSONObj authParams);

    /**
     * This method authenticates to another cluster member using appropriate
     * authentication data
     * @return true if the authentication was succesful
     */
    extern bool authenticateInternalUser(DBClientWithCommands* conn);

    /**
     * This method checks the validity of filename as a security key, hashes its
     * contents, and stores it in the internalSecurity variable.  Prints an
     * error message to the logs if there's an error.
     * @param filename the file containing the key
     * @return if the key was successfully stored
     */
    bool setUpSecurityKey(const std::string& filename);

} // namespace mongo

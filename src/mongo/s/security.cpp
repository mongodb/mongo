// security.cpp
/*
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

// security.cpp

#include "pch.h"

#include "mongo/db/auth/authorization_manager.h"
#include "../db/security_common.h"
#include "../db/security.h"
#include "config.h"
#include "client_info.h"
#include "grid.h"

// this is the _mongos only_ implementation of security.h

namespace mongo {

    bool CmdLogout::run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        AuthorizationManager* authManager = ClientInfo::get()->getAuthorizationManager();
        authManager->logoutDatabase(dbname);
        return true;
    }
}

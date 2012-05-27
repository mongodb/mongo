// connection_factory.cpp

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

#include "mongo/client/connpool.h"

namespace mongo {

    ScopedDbConnection* ScopedDbConnection::getScopedDbConnection() {
        return new ScopedDbConnection();
    }

    ScopedDbConnection* ScopedDbConnection::getScopedDbConnection(const string& host,
                                                                  double socketTimeout) {
        return new ScopedDbConnection(host, socketTimeout);
    }

    ScopedDbConnection* ScopedDbConnection::getScopedDbConnection(const string& host,
                                                                  DBClientBase* conn,
                                                                  double socketTimeout) {
        return new ScopedDbConnection(host, conn, socketTimeout);
    }

}

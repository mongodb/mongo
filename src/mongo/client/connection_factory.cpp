// connection_factory.cpp

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/client/connpool.h"

// This file contains the client-only implementation of the factory functions for getting
// ScopedDbConnections.
namespace mongo {

    ScopedDbConnection* ScopedDbConnection::getScopedDbConnection() {
        return new ScopedDbConnection();
    }

    ScopedDbConnection* ScopedDbConnection::getScopedDbConnection(const string& host,
                                                                  double socketTimeout) {
        return new ScopedDbConnection(host, socketTimeout);
    }

    ScopedDbConnection* ScopedDbConnection::getScopedDbConnection(const ConnectionString& host,
                                                                  double socketTimeout) {
        return new ScopedDbConnection(host, socketTimeout);
    }

    // In the client code, these functions are the same as the ones above, since we don't have to
    // do special handling of authentication for commands in the client.
    ScopedDbConnection* ScopedDbConnection::getInternalScopedDbConnection() {
        return getScopedDbConnection();
    }

    ScopedDbConnection* ScopedDbConnection::getInternalScopedDbConnection(const string& host,
                                                                          double socketTimeout) {
        return getScopedDbConnection( host, socketTimeout );
    }

    ScopedDbConnection* ScopedDbConnection::getInternalScopedDbConnection(const ConnectionString& host,
                                                                          double socketTimeout) {
        return getScopedDbConnection( host, socketTimeout );
    }

}

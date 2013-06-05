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

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/server_parameters.h"

namespace mongo {
namespace {

    MONGO_EXPORT_STARTUP_SERVER_PARAMETER(supportCompatibilityFormPrivilegeDocuments, bool, true);

    MONGO_INITIALIZER(AuthSetPrivilegeDocumentCompatibilitySupport)(InitializerContext*) {
        AuthorizationManager::setSupportOldStylePrivilegeDocuments(
                supportCompatibilityFormPrivilegeDocuments);
        return Status::OK();
    }

}  // namespace
}  // namespace mongo

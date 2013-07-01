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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/principal_set.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

    // Server parameter controlling whether or not user ids are included in log entries.
    MONGO_EXPORT_STARTUP_SERVER_PARAMETER(logUserIds, bool, false);

    /**
     * Note: When appending new strings to the builder, make sure to pass false to the
     * includeEndingNull parameter.
     */
    void appendServerExtraLogContext(BufBuilder& builder) {
        ClientBasic* clientBasic = ClientBasic::getCurrent();
        if (!clientBasic)
            return;
        if (!clientBasic->hasAuthorizationSession())
            return;

        PrincipalSet::NameIterator principals =
            clientBasic->getAuthorizationSession()->getAuthenticatedPrincipalNames();

        if (!principals.more())
            return;

        builder.appendStr("user:", false);
        builder.appendStr(principals.next().toString(), false);
        while (principals.more()) {
            builder.appendChar(',');
            builder.appendStr(principals.next().toString(), false);
        }
        builder.appendChar(' ');
    }

    MONGO_INITIALIZER_WITH_PREREQUISITES(SetServerLogContextFunction,
                                         MONGO_NO_PREREQUISITES)(InitializerContext*) {
        if (!logUserIds)
            return Status::OK();

        return logger::registerExtraLogContextFn(appendServerExtraLogContext);
    }

}  // namespace
}  // namespace mongo

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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_set.h"
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
    if (!AuthorizationSession::exists(clientBasic))
        return;

    UserNameIterator users = AuthorizationSession::get(clientBasic)->getAuthenticatedUserNames();

    if (!users.more())
        return;

    builder.appendStr("user:", false);
    builder.appendStr(users.next().toString(), false);
    while (users.more()) {
        builder.appendChar(',');
        builder.appendStr(users.next().toString(), false);
    }
    builder.appendChar(' ');
}

MONGO_INITIALIZER(SetServerLogContextFunction)(InitializerContext*) {
    if (!logUserIds)
        return Status::OK();

    return logger::registerExtraLogContextFn(appendServerExtraLogContext);
}

}  // namespace
}  // namespace mongo

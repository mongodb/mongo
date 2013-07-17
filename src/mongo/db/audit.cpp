/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/audit.h"

#if MONGO_ENTERPRISE_VERSION
#define MONGO_AUDIT_STUB ;
#else
#define MONGO_AUDIT_STUB {}
#endif

namespace mongo {
namespace audit {

    void logCommandAuthzCheck(ClientBasic* client,
                              const NamespaceString& ns,
                              const BSONObj& cmdObj,
                              ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logDeleteAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& pattern,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logFsyncUnlockAuthzCheck(
            ClientBasic* client,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logGetMoreAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            long long cursorId,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logInProgAuthzCheck(
            ClientBasic* client,
            const BSONObj& filter,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logInsertAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& insertedObj,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logKillCursorsAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            long long cursorId,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logKillOpAuthzCheck(
            ClientBasic* client,
            const BSONObj& filter,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logQueryAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& query,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

    void logUpdateAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& query,
            const BSONObj& updateObj,
            bool isUpsert,
            bool isMulti,
            ErrorCodes::Error result) MONGO_AUDIT_STUB

}  // namespace audit
}  // namespace mongo


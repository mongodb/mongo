/*
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata/audit_metadata.h"

#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {
namespace rpc {

const OperationContext::Decoration<AuditMetadata> AuditMetadata::get =
    OperationContext::declareDecoration<AuditMetadata>();

AuditMetadata::AuditMetadata(boost::optional<UsersAndRoles> impersonatedUsersAndRoles)
    : _impersonatedUsersAndRoles(std::move(impersonatedUsersAndRoles)) {}

#if !defined(MONGO_ENTERPRISE_VERSION)

StatusWith<AuditMetadata> AuditMetadata::readFromMetadata(const BSONObj&) {
    return AuditMetadata{};
}

StatusWith<AuditMetadata> AuditMetadata::readFromMetadata(const BSONElement&) {
    return AuditMetadata{};
}

Status AuditMetadata::writeToMetadata(BSONObjBuilder*) const {
    return Status::OK();
}

Status AuditMetadata::downconvert(const BSONObj& command,
                                  const BSONObj&,
                                  BSONObjBuilder* commandBob,
                                  int*) {
    commandBob->appendElements(command);
    return Status::OK();
}

Status AuditMetadata::upconvert(const BSONObj& command,
                                const int,
                                BSONObjBuilder* commandBob,
                                BSONObjBuilder*) {
    commandBob->appendElements(command);
    return Status::OK();
}

#endif

const boost::optional<AuditMetadata::UsersAndRoles>& AuditMetadata::getImpersonatedUsersAndRoles()
    const {
    return _impersonatedUsersAndRoles;
}

const char kLegacyImpersonatedUsersFieldName[] = "impersonatedUsers";
const char kLegacyImpersonatedRolesFieldName[] = "impersonatedRoles";

}  // namespace rpc
}  // namespace mongo

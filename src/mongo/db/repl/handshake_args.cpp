/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/handshake_args.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

namespace {

const std::string kRIDFieldName = "handshake";
// TODO(danneberg) remove after 3.0 since this field is only allowed for backwards compatibility
const std::string kOldMemberConfigFieldName = "config";
const std::string kMemberIdFieldName = "member";

const std::string kLegalHandshakeFieldNames[] = {
    kRIDFieldName, kOldMemberConfigFieldName, kMemberIdFieldName};

}  // namespace

HandshakeArgs::HandshakeArgs() : _hasRid(false), _hasMemberId(false), _rid(OID()), _memberId(-1) {}

Status HandshakeArgs::initialize(const BSONObj& argsObj) {
    Status status = bsonCheckOnlyHasFields("HandshakeArgs", argsObj, kLegalHandshakeFieldNames);
    if (!status.isOK())
        return status;

    BSONElement oid;
    status = bsonExtractTypedField(argsObj, kRIDFieldName, jstOID, &oid);
    if (!status.isOK())
        return status;
    _rid = oid.OID();
    _hasRid = true;

    status = bsonExtractIntegerField(argsObj, kMemberIdFieldName, &_memberId);
    if (!status.isOK()) {
        // field not necessary for master slave, do not return NoSuchKey Error
        if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
        _memberId = -1;
    } else {
        _hasMemberId = true;
    }

    return Status::OK();
}

bool HandshakeArgs::isInitialized() const {
    return _hasRid;
}

void HandshakeArgs::setRid(const OID& newVal) {
    _rid = newVal;
    _hasRid = true;
}

void HandshakeArgs::setMemberId(long long newVal) {
    _memberId = newVal;
    _hasMemberId = true;
}

BSONObj HandshakeArgs::toBSON() const {
    invariant(isInitialized());
    BSONObjBuilder builder;
    builder.append(kRIDFieldName, _rid);
    builder.append(kMemberIdFieldName, _memberId);
    return builder.obj();
}

}  // namespace repl
}  // namespace mongo

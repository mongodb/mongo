/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/s/stale_exception.h"

#include "mongo/base/init.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleConfigInfo);
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(StaleDbRoutingVersion);

}  // namespace

StaleConfigInfo::StaleConfigInfo(const BSONObj& obj)
    : StaleConfigInfo(NamespaceString(obj["ns"].type() == String ? obj["ns"].String() : ""),
                      ChunkVersion::fromBSON(obj, "vReceived"),
                      ChunkVersion::fromBSON(obj, "vWanted")) {}

void StaleConfigInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("ns", _nss.ns());
    _received.addToBSON(*bob, "vReceived");
    _wanted.addToBSON(*bob, "vWanted");
}

std::shared_ptr<const ErrorExtraInfo> StaleConfigInfo::parse(const BSONObj& obj) {
    return std::make_shared<StaleConfigInfo>(obj);
}

StaleDbRoutingVersion::StaleDbRoutingVersion(const BSONObj& obj)
    : StaleDbRoutingVersion(
          obj["db"].String(),
          DatabaseVersion::parse(IDLParserErrorContext("StaleDbRoutingVersion-vReceived"),
                                 obj["vReceived"].Obj()),
          !obj["vWanted"].eoo()
              ? DatabaseVersion::parse(IDLParserErrorContext("StaleDbRoutingVersion-vWanted"),
                                       obj["vWanted"].Obj())
              : boost::optional<DatabaseVersion>{}) {}

void StaleDbRoutingVersion::serialize(BSONObjBuilder* bob) const {
    bob->append("db", _db);
    bob->append("vReceived", _received.toBSON());
    if (_wanted) {
        bob->append("vWanted", _wanted->toBSON());
    }
}

std::shared_ptr<const ErrorExtraInfo> StaleDbRoutingVersion::parse(const BSONObj& obj) {
    return std::make_shared<StaleDbRoutingVersion>(obj);
}

}  // namespace mongo

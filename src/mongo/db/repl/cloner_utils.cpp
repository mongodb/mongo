/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo {
namespace repl {

BSONObj ClonerUtils::makeTenantDatabaseRegex(StringData prefix) {
    return BSON("$regex"
                << "^" + prefix + "_");
}

BSONObj ClonerUtils::makeTenantDatabaseFilter(StringData prefix) {
    return BSON("name" << makeTenantDatabaseRegex(prefix));
}

BSONObj ClonerUtils::buildMajorityWaitRequest(Timestamp operationTime) {
    BSONObjBuilder bob;
    bob.append("find",
               NamespaceStringUtil::serialize(NamespaceString::kSystemReplSetNamespace,
                                              SerializationContext::stateDefault()));
    bob.append("filter", BSONObj());
    ReadConcernArgs readConcern(LogicalTime(operationTime), ReadConcernLevel::kMajorityReadConcern);
    readConcern.appendInfo(&bob);
    return bob.obj();
}

bool ClonerUtils::isDatabaseForTenant(const DatabaseName& db,
                                      const boost::optional<TenantId>& tenant,
                                      MigrationProtocolEnum protocol) {
    if (auto tenantId = db.tenantId()) {
        return tenantId == *tenant;
    }

    // If we are not running in multitenancy mode, then it's possible that the `dbName` has a prefix
    // which hasn't been parsed into the DatabaseName type. Serialize `dbName` to a string, and
    // look for a tenant id manually.
    auto fullDbName = DatabaseNameUtil::serialize(db, SerializationContext::stateDefault());
    auto tenantDelim = fullDbName.find('_');
    if (tenantDelim != std::string::npos) {
        return (*tenant).toString() == fullDbName.substr(0, tenantDelim);
    }
    return false;
}

}  // namespace repl
}  // namespace mongo

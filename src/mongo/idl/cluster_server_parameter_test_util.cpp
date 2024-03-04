/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/idl/cluster_server_parameter_test_util.h"

#include <boost/move/utility_core.hpp>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/idl/cluster_server_parameter_test_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace cluster_server_parameter_test_util {

const TenantId ClusterServerParameterTestBase::kTenantId =
    TenantId(OID("123456789012345678901234"_sd));

void upsert(BSONObj doc, const boost::optional<TenantId>& tenantId) {
    const auto kMajorityWriteConcern = BSON("writeConcern" << BSON("w"
                                                                   << "majority"));

    auto uniqueOpCtx = cc().makeOperationContext();
    auto* opCtx = uniqueOpCtx.get();
    auth::ValidatedTenancyScopeGuard::runAsTenant(opCtx, tenantId, [&]() {
        DBDirectClient client(opCtx);

        auto opMsgRequest = OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::get(opCtx),
            DatabaseName::createDatabaseName_forTest(tenantId, "config"),
            [&] {
                write_ops::UpdateCommandRequest updateOp(
                    NamespaceString::makeClusterParametersNSS(tenantId));
                updateOp.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(BSON(ClusterServerParameter::k_idFieldName << kCSPTest));
                    entry.setU(
                        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$set" << doc)));
                    entry.setMulti(false);
                    entry.setUpsert(true);
                    return entry;
                }()});
                return updateOp.toBSON(kMajorityWriteConcern);
            }());

        auto res = client.runCommand(opMsgRequest)->getCommandReply();

        BatchedCommandResponse response;
        std::string errmsg;
        if (!response.parseBSON(res, &errmsg)) {
            uasserted(ErrorCodes::FailedToParse, str::stream() << "Failure: " << errmsg);
        }

        uassertStatusOK(response.toStatus());
        uassert(ErrorCodes::OperationFailed, "No documents upserted", response.getN());
    });
}

void remove(const boost::optional<TenantId>& tenantId) {
    auto uniqueOpCtx = cc().makeOperationContext();
    auto* opCtx = uniqueOpCtx.get();
    auth::ValidatedTenancyScopeGuard::runAsTenant(opCtx, tenantId, [&]() {
        auto opMsgRequest = OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::get(opCtx),
            DatabaseName::createDatabaseName_forTest(tenantId, "config"),
            [&] {
                write_ops::DeleteCommandRequest deleteOp(
                    NamespaceString::makeClusterParametersNSS(tenantId));
                deleteOp.setDeletes({[] {
                    write_ops::DeleteOpEntry entry;
                    entry.setQ(BSON(ClusterServerParameter::k_idFieldName << kCSPTest));
                    entry.setMulti(true);
                    return entry;
                }()});
                return deleteOp.toBSON({});
            }());

        auto res = DBDirectClient(opCtx).runCommand(opMsgRequest)->getCommandReply();

        BatchedCommandResponse response;
        std::string errmsg;
        if (!response.parseBSON(res, &errmsg)) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Failed to parse reply to delete command: " << errmsg);
        }
        uassertStatusOK(response.toStatus());
    });
}

BSONObj makeClusterParametersDoc(const LogicalTime& cpTime,
                                 int intValue,
                                 StringData strValue,
                                 StringData parameterName) {
    ClusterServerParameter csp;
    csp.set_id(parameterName);
    csp.setClusterParameterTime(cpTime);

    ClusterServerParameterTest cspt;
    cspt.setClusterServerParameter(std::move(csp));
    cspt.setIntValue(intValue);
    cspt.setStrValue(strValue);

    return cspt.toBSON();
}

}  // namespace cluster_server_parameter_test_util
}  // namespace mongo

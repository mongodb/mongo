// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_gen.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace cluster_server_parameter_test_util {
using namespace std::literals::string_view_literals;

const TenantId ClusterServerParameterTestBase::kTenantId =
    TenantId(OID("123456789012345678901234"sv));

void upsert(BSONObj doc, const boost::optional<TenantId>& tenantId) {
    const auto kMajorityWriteConcern = BSON("writeConcern" << BSON("w" << "majority"));

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
                updateOp.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
                return updateOp.toBSON();
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
                return deleteOp.toBSON();
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
                                 std::string_view strValue,
                                 std::string_view parameterName) {
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

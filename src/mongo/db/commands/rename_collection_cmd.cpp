/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <algorithm>
#include <iosfwd>
#include <memory>
#include <string>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection_common.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using std::min;
using std::string;
using std::stringstream;

namespace {

class CmdRenameCollection final : public TypedCommand<CmdRenameCollection> {
public:
    using Request = RenameCollectionCommand;

    virtual bool adminOnly() const {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return " example: { renameCollection: foo.a, to: bar.b }";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void typedRun(OperationContext* opCtx) {
            const auto& fromNss = ns();
            const auto& toNss = request().getTo();

            uassert(ErrorCodes::IllegalOperation,
                    "Can't rename a collection to itself",
                    fromNss != toNss);

            RenameCollectionOptions options;
            options.stayTemp = request().getStayTemp();
            options.expectedSourceUUID = request().getCollectionUUID();
            visit(OverloadedVisitor{
                      [&options](bool dropTarget) { options.dropTarget = dropTarget; },
                      [&options](const UUID& uuid) {
                          options.dropTarget = true;
                          options.expectedTargetUUID = uuid;
                      },
                  },
                  request().getDropTarget());

            validateAndRunRenameCollection(opCtx, fromNss, toNss, options);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(rename_collection::checkAuthForRenameCollectionCommand(
                opCtx->getClient(), request()));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdRenameCollection).forShard();

}  // namespace
}  // namespace mongo

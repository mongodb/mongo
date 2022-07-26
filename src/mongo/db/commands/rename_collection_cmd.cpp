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


#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection_common.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using std::min;
using std::string;
using std::stringstream;

namespace {

class CmdRenameCollection : public ErrmsgCommandDeprecated {
public:
    CmdRenameCollection() : ErrmsgCommandDeprecated("renameCollection") {}
    virtual bool adminOnly() const {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
    }
    std::string help() const override {
        return " example: { renameCollection: foo.a, to: bar.b }";
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        auto renameRequest =
            RenameCollectionCommand::parse(IDLParserContext("renameCollection"), cmdObj);

        const auto& fromNss = renameRequest.getCommandParameter();
        const auto& toNss = renameRequest.getTo();

        uassert(
            ErrorCodes::IllegalOperation, "Can't rename a collection to itself", fromNss != toNss);

        RenameCollectionOptions options;
        options.stayTemp = renameRequest.getStayTemp();
        options.expectedSourceUUID = renameRequest.getCollectionUUID();
        stdx::visit(
            OverloadedVisitor{
                [&options](bool dropTarget) { options.dropTarget = dropTarget; },
                [&options](const UUID& uuid) {
                    options.dropTarget = true;
                    options.expectedTargetUUID = uuid;
                },
            },
            renameRequest.getDropTarget());

        validateAndRunRenameCollection(
            opCtx, renameRequest.getCommandParameter(), renameRequest.getTo(), options);
        return true;
    }

} cmdrenamecollection;

}  // namespace
}  // namespace mongo

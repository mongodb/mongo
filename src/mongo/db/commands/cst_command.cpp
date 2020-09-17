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

#include "mongo/base/init.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/parser_gen.hpp"

namespace mongo {

class CstCommand : public BasicCommand {
public:
    CstCommand() : BasicCommand("cst") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmdObj) const override {
        return false;
    }

    // Test commands should never be enabled in production, but we try to require auth on new
    // test commands anyway, just in case someone enables them by mistake.
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
        // This auth check is more restrictive than necessary, to make it simpler.
        // The CST command constructs a Pipeline, which might hold execution resources.
        // We could do fine-grained permission checking similar to the find or aggregate commands,
        // but that seems more complicated than necessary since this is only a test command.
        if (!authSession->isAuthorizedForAnyActionOnAnyResourceInDB(dbname)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string help() const override {
        return "test command for CST";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        CNode pipelineCst;
        {
            BSONLexer lexer(cmdObj["pipeline"].Obj(), ParserGen::token::START_PIPELINE);
            ParserGen parser(lexer, &pipelineCst);
            int err = parser.parse();
            // We use exceptions instead of return codes to report parse errors, so this
            // should never happen.
            invariant(!err);
        }
        result.append("cst", BSONArray(pipelineCst.toBson()));

        auto nss = NamespaceString{dbname, ""};
        auto expCtx = make_intrusive<ExpressionContext>(opCtx, nullptr /*collator*/, nss);
        auto pipeline = cst_pipeline_translation::translatePipeline(pipelineCst, expCtx);
        result.append("ds", pipeline->serializeToBson());

        return true;
    }
};
MONGO_REGISTER_TEST_COMMAND(CstCommand);

}  // namespace mongo

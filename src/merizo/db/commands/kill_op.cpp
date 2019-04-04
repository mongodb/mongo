/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kCommand

#include "merizo/platform/basic.h"

#include <limits>

#include "merizo/base/init.h"
#include "merizo/base/status.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/db/audit.h"
#include "merizo/db/auth/authorization_session.h"
#include "merizo/db/client.h"
#include "merizo/db/commands.h"
#include "merizo/db/commands/kill_op_cmd_base.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/service_context.h"
#include "merizo/util/log.h"
#include "merizo/util/merizoutils/str.h"

namespace merizo {

class KillOpCommand : public KillOpCmdBase {
public:
    bool run(OperationContext* opCtx,
             const std::string& db,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        long long opId = KillOpCmdBase::parseOpId(cmdObj);

        // Used by tests to check if auth checks passed.
        result.append("info", "attempting to kill op");
        log() << "going to kill op: " << opId;
        KillOpCmdBase::killLocalOperation(opCtx, opId);

        // killOp always reports success once past the auth check.
        return true;
    }
} killOpCmd;

}  // namespace merizo

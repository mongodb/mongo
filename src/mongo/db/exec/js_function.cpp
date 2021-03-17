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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/js_function.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/scripting/engine.h"

namespace mongo {

namespace {
std::string getAuthenticatedUserNamesToken(Client* client) {
    StringBuilder sb;

    auto as = AuthorizationSession::get(client);
    for (auto nameIter = as->getAuthenticatedUserNames(); nameIter.more(); nameIter.next()) {
        // Using a NUL byte which isn't valid in usernames to separate them.
        sb << '\0' << nameIter->getUnambiguousName();
    }

    return sb.str();
}
}  // namespace

JsFunction::JsFunction(OperationContext* opCtx,
                       const std::string& code,
                       const std::string& dbName) {
    invariant(opCtx != nullptr);
    uassert(
        ErrorCodes::BadValue, "no globalScriptEngine in $where parsing", getGlobalScriptEngine());

    uassert(ErrorCodes::BadValue, "ns for $where cannot be empty", dbName.size() != 0);

    const auto userToken = getAuthenticatedUserNamesToken(opCtx->getClient());
    _scope = getGlobalScriptEngine()->getPooledScope(opCtx, dbName, "where" + userToken);
    const auto guard = makeGuard([&] { _scope->unregisterOperation(); });

    _func = _scope->createFunction(code.c_str());
    uassert(ErrorCodes::BadValue, "$where compile error", _func);
}

bool JsFunction::runAsPredicate(const BSONObj& obj) const {
    _scope->registerOperation(Client::getCurrent()->getOperationContext());
    const auto scopeOpCtxGuard = makeGuard([&] { _scope->unregisterOperation(); });

    _scope->advanceGeneration();
    _scope->setObject("obj", obj);
    _scope->setBoolean("fullObject", true);  // this is a hack b/c fullObject used to be relevant

    auto err =
        _scope->invoke(_func, nullptr, &obj, internalQueryJavaScriptFnTimeoutMillis.load(), false);
    if (err == -3) {  // INVOKE_ERROR
        std::stringstream ss;
        ss << "error on invocation of $where function:\n" << _scope->getError();
        uassert(5038802, ss.str(), false);
    } else if (err != 0) {  // !INVOKE_SUCCESS
        uassert(5038803, "unknown error in invocation of $where function", false);
    }

    return _scope->getBoolean("__returnValue");
}

}  // namespace mongo

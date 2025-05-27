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

#include "mongo/db/exec/js_function.h"

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"

#include <ostream>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
std::string getAuthenticatedUserNamesToken(Client* client) {
    StringBuilder sb;

    auto as = AuthorizationSession::get(client);
    if (auto name = as->getAuthenticatedUserName()) {
        // Using a NUL byte which isn't valid in usernames to separate them.
        if (const auto& tenant = name->tenantId()) {
            sb << '\0' << tenant->toString();
        }
        sb << '\0' << name->getUnambiguousName();
    }

    return sb.str();
}
}  // namespace

JsFunction::JsFunction(OperationContext* opCtx, std::string code, const DatabaseName& dbName) {
    _init(opCtx, std::move(code), dbName);
}

JsFunction::JsFunction(const JsFunction& other) {
    _init(Client::getCurrent()->getOperationContext(), other._code, other._dbName);
}

JsFunction& JsFunction::operator=(const JsFunction& other) {
    if (this != &other) {
        _init(Client::getCurrent()->getOperationContext(), other._code, other._dbName);
    }
    return *this;
}

void JsFunction::_init(OperationContext* opCtx, std::string code, const DatabaseName& dbName) {
    invariant(opCtx != nullptr);
    uassert(6108304, "no globalScriptEngine in $where parsing", getGlobalScriptEngine());
    uassert(6108305, "ns for $where cannot be empty", !dbName.isEmpty());

    _code = std::move(code);
    _dbName = dbName;

    const auto userToken = getAuthenticatedUserNamesToken(opCtx->getClient());
    _scope = getGlobalScriptEngine()->getPooledScope(opCtx, _dbName, "where" + userToken);
    const ScopeGuard guard([&] { _scope->unregisterOperation(); });

    _func = _scope->createFunction(_code.c_str());
    uassert(6108306, "$where compile error", _func);
}

bool JsFunction::runAsPredicate(const BSONObj& obj) const {
    _scope->registerOperation(Client::getCurrent()->getOperationContext());
    const ScopeGuard scopeOpCtxGuard([&] { _scope->unregisterOperation(); });

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

size_t JsFunction::getApproximateSize() const {
    // The memory pointed to by _scope is owned by the MozJS engine, so we do not account
    // for the size of that memory here.
    return sizeof(JsFunction);
}
}  // namespace mongo

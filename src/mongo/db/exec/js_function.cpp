// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/js_function.h"

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"

#include <utility>


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
    return _scope->execPredicate(_func, obj, internalQueryJavaScriptFnTimeoutMillis.load());
}

size_t JsFunction::getApproximateSize() const {
    // The memory pointed to by _scope is owned by the MozJS engine, so we do not account
    // for the size of that memory here.
    return sizeof(JsFunction);
}
}  // namespace mongo

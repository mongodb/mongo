// expression_where.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_where.h"

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"


namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;
using stdx::make_unique;

WhereMatchExpression::WhereMatchExpression(OperationContext* txn, WhereParams params)
    : WhereMatchExpressionBase(std::move(params)), _txn(txn) {
    invariant(_txn != NULL);

    _func = 0;
}

Status WhereMatchExpression::init(StringData dbName) {
    if (!globalScriptEngine) {
        return Status(ErrorCodes::BadValue, "no globalScriptEngine in $where parsing");
    }

    if (dbName.size() == 0) {
        return Status(ErrorCodes::BadValue, "ns for $where cannot be empty");
    }

    _dbName = dbName.toString();

    const string userToken =
        AuthorizationSession::get(ClientBasic::getCurrent())->getAuthenticatedUserNamesToken();

    try {
        _scope = globalScriptEngine->getPooledScope(_txn, _dbName, "where" + userToken);
        _func = _scope->createFunction(getCode().c_str());
    } catch (...) {
        return exceptionToStatus();
    }

    if (!_func)
        return Status(ErrorCodes::BadValue, "$where compile error");

    return Status::OK();
}

bool WhereMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    uassert(28692, "$where compile error", _func);
    BSONObj obj = doc->toBSON();

    if (!getScope().isEmpty()) {
        _scope->init(&getScope());
    }

    _scope->advanceGeneration();
    _scope->setObject("obj", const_cast<BSONObj&>(obj));
    _scope->setBoolean("fullObject", true);  // this is a hack b/c fullObject used to be relevant

    int err = _scope->invoke(_func, 0, &obj, 1000 * 60, false);
    if (err == -3) {  // INVOKE_ERROR
        stringstream ss;
        ss << "error on invocation of $where function:\n" << _scope->getError();
        uassert(16812, ss.str(), false);
    } else if (err != 0) {  // ! INVOKE_SUCCESS
        uassert(16813, "unknown error in invocation of $where function", false);
    }

    return _scope->getBoolean("__returnValue") != 0;
}

unique_ptr<MatchExpression> WhereMatchExpression::shallowClone() const {
    WhereParams params;
    params.code = getCode();
    params.scope = getScope();
    unique_ptr<WhereMatchExpression> e = make_unique<WhereMatchExpression>(_txn, std::move(params));
    e->init(_dbName);
    if (getTag()) {
        e->setTag(getTag()->clone());
    }
    return std::move(e);
}
}

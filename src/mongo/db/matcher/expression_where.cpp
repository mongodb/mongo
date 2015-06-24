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

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"


namespace mongo {

using std::unique_ptr;
using std::endl;
using std::string;
using std::stringstream;
using stdx::make_unique;

class WhereMatchExpression : public MatchExpression {
public:
    WhereMatchExpression(OperationContext* txn) : MatchExpression(WHERE), _txn(txn) {
        invariant(_txn != NULL);

        _func = 0;
    }

    virtual ~WhereMatchExpression() {}

    Status init(StringData dbName, StringData theCode, const BSONObj& scope);

    virtual bool matches(const MatchableDocument* doc, MatchDetails* details = 0) const;

    virtual bool matchesSingleElement(const BSONElement& e) const {
        return false;
    }

    virtual unique_ptr<MatchExpression> shallowClone() const {
        unique_ptr<WhereMatchExpression> e = make_unique<WhereMatchExpression>(_txn);
        e->init(_dbName, _code, _userScope);
        if (getTag()) {
            e->setTag(getTag()->clone());
        }
        return std::move(e);
    }

    virtual void debugString(StringBuilder& debug, int level = 0) const;

    virtual void toBSON(BSONObjBuilder* out) const;

    virtual bool equivalent(const MatchExpression* other) const;

    virtual void resetTag() {
        setTag(NULL);
    }

private:
    string _dbName;
    string _code;
    BSONObj _userScope;

    unique_ptr<Scope> _scope;
    ScriptingFunction _func;

    // Not owned. See comments insde WhereCallbackReal for the lifetime of this pointer.
    OperationContext* _txn;
};

Status WhereMatchExpression::init(StringData dbName, StringData theCode, const BSONObj& scope) {
    if (dbName.size() == 0) {
        return Status(ErrorCodes::BadValue, "ns for $where cannot be empty");
    }

    if (theCode.size() == 0) {
        return Status(ErrorCodes::BadValue, "code for $where cannot be empty");
    }

    _dbName = dbName.toString();
    _code = theCode.toString();
    _userScope = scope.getOwned();

    const string userToken =
        AuthorizationSession::get(ClientBasic::getCurrent())->getAuthenticatedUserNamesToken();

    try {
        _scope = globalScriptEngine->getPooledScope(_txn, _dbName, "where" + userToken);
        _func = _scope->createFunction(_code.c_str());
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

    if (!_userScope.isEmpty()) {
        _scope->init(&_userScope);
    }

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

void WhereMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << "$where\n";

    _debugAddSpace(debug, level + 1);
    debug << "dbName: " << _dbName << "\n";

    _debugAddSpace(debug, level + 1);
    debug << "code: " << _code << "\n";

    _debugAddSpace(debug, level + 1);
    debug << "scope: " << _userScope << "\n";
}

void WhereMatchExpression::toBSON(BSONObjBuilder* out) const {
    out->append("$where", _code);
}

bool WhereMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;
    const WhereMatchExpression* realOther = static_cast<const WhereMatchExpression*>(other);
    return _dbName == realOther->_dbName && _code == realOther->_code &&
        _userScope == realOther->_userScope;
}

WhereCallbackReal::WhereCallbackReal(OperationContext* txn, StringData dbName)
    : _txn(txn), _dbName(dbName) {}

StatusWithMatchExpression WhereCallbackReal::parseWhere(const BSONElement& where) const {
    if (!globalScriptEngine)
        return StatusWithMatchExpression(ErrorCodes::BadValue,
                                         "no globalScriptEngine in $where parsing");

    unique_ptr<WhereMatchExpression> exp(new WhereMatchExpression(_txn));
    if (where.type() == String || where.type() == Code) {
        Status s = exp->init(_dbName, where.valuestr(), BSONObj());
        if (!s.isOK())
            return StatusWithMatchExpression(s);
        return StatusWithMatchExpression(exp.release());
    }

    if (where.type() == CodeWScope) {
        Status s =
            exp->init(_dbName, where.codeWScopeCode(), BSONObj(where.codeWScopeScopeDataUnsafe()));
        if (!s.isOK())
            return StatusWithMatchExpression(s);
        return StatusWithMatchExpression(exp.release());
    }

    return StatusWithMatchExpression(ErrorCodes::BadValue, "$where got bad type");
}
}

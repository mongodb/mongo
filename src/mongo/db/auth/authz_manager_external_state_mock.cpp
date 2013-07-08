/*
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
*/

#include "mongo/db/auth/authz_manager_external_state_mock.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    Status AuthzManagerExternalStateMock::insertPrivilegeDocument(const std::string& dbname,
                                                                  const BSONObj& userObj) const {
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::updatePrivilegeDocument(const UserName& user,
                                                                  const BSONObj& updateObj) const {
        return Status::OK();
    }

    Status AuthzManagerExternalStateMock::insertPrivilegeDocument(const std::string& dbname,
                                                                  const BSONObj& userObj) {
        _userDocuments.insert(make_pair(dbname, userObj));
        return Status::OK();
    }

    void AuthzManagerExternalStateMock::clearPrivilegeDocuments() {
        _userDocuments.clear();
    }

    bool AuthzManagerExternalStateMock::_findUser(const std::string& usersNamespace,
                           const BSONObj& query,
                           BSONObj* result) const {
        StatusWithMatchExpression parseResult = MatchExpressionParser::parse(query);
        if (!parseResult.isOK()) {
            return false;
        }
        MatchExpression* matcher = parseResult.getValue();

        for (unordered_map<std::string, BSONObj>::const_iterator it = _userDocuments.begin();
                it != _userDocuments.end(); ++it) {
            if (nsToDatabase(usersNamespace) == it->first && matcher->matchesBSON(it->second)) {
                *result = it->second;
                return true;
            }
        }
        return false;
    }

} // namespace mongo

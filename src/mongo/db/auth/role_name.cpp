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
 */

#include "mongo/db/auth/role_name.h"

#include <algorithm>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    RoleName::RoleName(const StringData& role, const StringData& dbname) {
        _fullName.resize(role.size() + dbname.size() + 1);
        std::string::iterator iter = std::copy(role.rawData(),
                                               role.rawData() + role.size(),
                                               _fullName.begin());
        *iter = '@';
        ++iter;
        iter = std::copy(dbname.rawData(), dbname.rawData() + dbname.size(), iter);
        dassert(iter == _fullName.end());
        _splitPoint = role.size();
    }

    RoleNameSetIterator::RoleNameSetIterator(const unordered_set<RoleName>::const_iterator& begin,
                                             const unordered_set<RoleName>::const_iterator& end) :
                                                _begin(begin), _end(end) {}

    RoleNameSetIterator::~RoleNameSetIterator() {};

    bool RoleNameSetIterator::more() const {
        return _begin != _end;
    }

    const RoleName& RoleNameSetIterator::next() {
        const RoleName& toReturn = get();
        ++_begin;
        return toReturn;
    }

    const RoleName& RoleNameSetIterator::get() const {
        return *_begin;
    }

    RoleNameIterator::Impl* RoleNameSetIterator::doClone() const {
        return new RoleNameSetIterator(_begin, _end);
    }

}  // namespace mongo

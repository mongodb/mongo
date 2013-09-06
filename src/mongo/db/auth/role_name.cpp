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

    std::ostream& operator<<(std::ostream& os, const RoleName& name) {
        return os << name.getFullName();
    }

}  // namespace mongo

/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/auth/user_set.h"

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user.h"

namespace mongo {

namespace {
class UserSetNameIteratorImpl : public UserNameIterator::Impl {
    MONGO_DISALLOW_COPYING(UserSetNameIteratorImpl);

public:
    UserSetNameIteratorImpl(const UserSet::iterator& begin, const UserSet::iterator& end)
        : _curr(begin), _end(end) {}
    virtual ~UserSetNameIteratorImpl() {}
    virtual bool more() const {
        return _curr != _end;
    }
    virtual const UserName& next() {
        return (*(_curr++))->getName();
    }
    virtual const UserName& get() const {
        return (*_curr)->getName();
    }
    virtual UserNameIterator::Impl* doClone() const {
        return new UserSetNameIteratorImpl(_curr, _end);
    }

private:
    UserSet::iterator _curr;
    UserSet::iterator _end;
};
}  // namespace

UserSet::UserSet() : _users(), _usersEnd(_users.end()) {}
UserSet::~UserSet() {}

User* UserSet::add(User* user) {
    for (mutable_iterator it = mbegin(); it != mend(); ++it) {
        User* current = *it;
        if (current->getName().getDB() == user->getName().getDB()) {
            // There can be only one user per database.
            *it = user;
            return current;
        }
    }
    if (_usersEnd == _users.end()) {
        _users.push_back(user);
        _usersEnd = _users.end();
    } else {
        *_usersEnd = user;
        ++_usersEnd;
    }
    return NULL;
}

User* UserSet::removeByDBName(StringData dbname) {
    for (iterator it = begin(); it != end(); ++it) {
        User* current = *it;
        if (current->getName().getDB() == dbname) {
            return removeAt(it);
        }
    }
    return NULL;
}

User* UserSet::replaceAt(iterator it, User* replacement) {
    size_t offset = it - begin();
    User* old = _users[offset];
    _users[offset] = replacement;
    return old;
}

User* UserSet::removeAt(iterator it) {
    size_t offset = it - begin();
    User* old = _users[offset];
    --_usersEnd;
    _users[offset] = *_usersEnd;
    *_usersEnd = NULL;
    return old;
}

User* UserSet::lookup(const UserName& name) const {
    User* user = lookupByDBName(name.getDB());
    if (user && user->getName() == name) {
        return user;
    }
    return NULL;
}

User* UserSet::lookupByDBName(StringData dbname) const {
    for (iterator it = begin(); it != end(); ++it) {
        User* current = *it;
        if (current->getName().getDB() == dbname) {
            return current;
        }
    }
    return NULL;
}

UserNameIterator UserSet::getNames() const {
    return UserNameIterator(new UserSetNameIteratorImpl(begin(), end()));
}
}  // namespace mongo

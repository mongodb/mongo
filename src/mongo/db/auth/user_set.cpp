/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/auth/user_set.h"

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user.h"

namespace mongo {

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
        }
        else {
            *_usersEnd = user;
            ++_usersEnd;
        }
        return NULL;
    }

    User* UserSet::removeByDBName(const StringData& dbname) {
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

    User* UserSet::lookupByDBName(const StringData& dbname) const {
        for (iterator it = begin(); it != end(); ++it) {
            User* current = *it;
            if (current->getName().getDB() == dbname) {
                return current;
            }
        }
        return NULL;
    }

} // namespace mongo

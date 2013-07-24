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

    UserSet::UserSet() {}
    UserSet::~UserSet() {}

    User* UserSet::add(User* user) {
        for (std::vector<User*>::iterator it = _users.begin();
                it != _users.end(); ++it) {
            User* current = *it;
            if (current->getName().getDB() == user->getName().getDB()) {
                // There can be only one user per database.
                *it = user;
                return current;
            }
        }
        _users.push_back(user);
        return NULL;
    }

    User* UserSet::removeByDBName(const StringData& dbname) {
        for (std::vector<User*>::iterator it = _users.begin();
                it != _users.end(); ++it) {
            User* current = *it;
            if (current->getName().getDB() == dbname) {
                _users.erase(it);
                return current;
            }
        }
        return NULL;
    }

    User* UserSet::lookup(const UserName& name) const {
        User* user = lookupByDBName(name.getDB());
        if (user && user->getName() == name) {
            return user;
        }
        return NULL;
    }

    User* UserSet::lookupByDBName(const StringData& dbname) const {
        for (std::vector<User*>::const_iterator it = _users.begin();
                it != _users.end(); ++it) {
            User* current = *it;
            if (current->getName().getDB() == dbname) {
                return current;
            }
        }
        return NULL;
    }

} // namespace mongo

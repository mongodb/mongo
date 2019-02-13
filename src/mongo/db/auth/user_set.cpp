/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/auth/user_set.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user.h"

namespace mongo {

namespace {
class UserSetNameIteratorImpl : public UserNameIterator::Impl {
    MONGO_DISALLOW_COPYING(UserSetNameIteratorImpl);

public:
    UserSetNameIteratorImpl(const UserSet::const_iterator& begin,
                            const UserSet::const_iterator& end)
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
    UserSet::const_iterator _curr;
    UserSet::const_iterator _end;
};
}  // namespace

void UserSet::add(UserHandle user) {
    auto it = std::find_if(_users.begin(), _users.end(), [&](const auto& storedUser) {
        return user->getName().getDB() == storedUser->getName().getDB();
    });
    if (it == _users.end()) {
        _users.push_back(std::move(user));
    } else {
        *it = std::move(user);
    }
}

void UserSet::removeByDBName(StringData dbname) {
    auto it = std::find_if(_users.begin(), _users.end(), [&](const auto& user) {
        return user->getName().getDB() == dbname;
    });
    if (it != _users.end()) {
        _users.erase(it);
    }
}

void UserSet::replaceAt(iterator it, UserHandle replacement) {
    *it = std::move(replacement);
}

void UserSet::removeAt(iterator it) {
    _users.erase(it);
}

UserHandle UserSet::lookup(const UserName& name) const {
    auto it = std::find_if(_users.begin(), _users.end(), [&](const auto& user) {
        invariant(user);
        return user->getName() == name;
    });

    return (it != _users.end()) ? *it : nullptr;
}

UserHandle UserSet::lookupByDBName(StringData dbname) const {
    auto it = std::find_if(_users.begin(), _users.end(), [&](const auto& user) {
        invariant(user);
        return user->getName().getDB() == dbname;
    });
    return (it != _users.end()) ? *it : nullptr;
}

UserNameIterator UserSet::getNames() const {
    return UserNameIterator(
        std::make_unique<UserSetNameIteratorImpl>(_users.cbegin(), _users.cend()));
}
}  // namespace mongo

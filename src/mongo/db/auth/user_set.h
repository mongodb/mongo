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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"


namespace mongo {

/**
 * A collection of authenticated users.
 * This class does not do any locking/synchronization, the consumer will be responsible for
 * synchronizing access.
 */
class UserSet {
    MONGO_DISALLOW_COPYING(UserSet);

public:
    typedef std::vector<User*>::const_iterator iterator;

    UserSet();
    ~UserSet();

    /**
     * Adds a User to the UserSet.
     *
     * The UserSet does not take ownership of the User.
     *
     * As there can only be one user per database in the UserSet, if a User already exists for
     * the new User's database, the old user will be removed from the set and returned.  It is
     * the caller's responsibility to then release that user.  If no user already exists for the
     * new user's database, returns NULL.
     *
     * Invalidates any outstanding iterators or NameIterators.
     */
    User* add(User* user);

    /**
     * Replaces the user at "it" with "replacement."  Does not take ownership of the User.
     * Returns a pointer to the old user referenced by "it".  Does _not_ invalidate "iterator"
     * instances.
     */
    User* replaceAt(iterator it, User* replacement);

    /**
     * Removes the user at "it", and returns a pointer to it.  After this call, "it" remains
     * valid.  It will either equal "end()", or refer to some user between the values of "it"
     * and "end()" before this call was made.
     */
    User* removeAt(iterator it);

    /**
     * Removes the User whose authentication credentials came from dbname, and returns that
     * user.  It is the caller's responsibility to then release that user back to the
     * authorizationManger.  If no user exists for the given database, returns NULL;
     */
    User* removeByDBName(StringData dbname);

    // Returns the User with the given name, or NULL if not found.
    // Ownership of the returned User remains with the UserSet.  The pointer
    // returned is only guaranteed to remain valid until the next non-const method is called
    // on the UserSet.
    User* lookup(const UserName& name) const;

    // Gets the user whose authentication credentials came from dbname, or NULL if none
    // exist.  There should be at most one such user.
    User* lookupByDBName(StringData dbname) const;

    // Gets an iterator over the names of the users stored in the set.  The iterator is
    // valid until the next non-const method is called on the UserSet.
    UserNameIterator getNames() const;

    iterator begin() const {
        return _users.begin();
    }
    iterator end() const {
        return _usersEnd;
    }

private:
    typedef std::vector<User*>::iterator mutable_iterator;

    mutable_iterator mbegin() {
        return _users.begin();
    }
    mutable_iterator mend() {
        return _usersEnd;
    }

    // The UserSet maintains ownership of the Users in it, and is responsible for
    // returning them to the AuthorizationManager when done with them.
    std::vector<User*> _users;
    std::vector<User*>::iterator _usersEnd;
};

}  // namespace mongo

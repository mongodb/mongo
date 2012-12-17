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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/principal.h"
#include "mongo/db/auth/principal_name.h"

namespace mongo {

    /**
     * A collection of authenticated principals.
     * This class does not do any locking/synchronization, the consumer will be responsible for
     * synchronizing access.
     */
    class PrincipalSet {
        MONGO_DISALLOW_COPYING(PrincipalSet);
    public:
        typedef std::vector<Principal*>::const_iterator iterator;

        /**
         * Forward iterator over the names of the principals stored in a PrincipalSet.
         *
         * Instances are valid until the underlying vector<Principal*> is modified.
         *
         * more() must be the first method called after construction, and must be checked
         * after each call to next() before calling any other methods.
         */
        class NameIterator {
        public:
            explicit NameIterator(const std::vector<Principal*>& principals) :
                _curr(principals.begin()),
                _end(principals.end()) {
            }

            NameIterator() {}

            bool more() { return _curr != _end; }
            const PrincipalName& next() {
                const PrincipalName& ret = get();
                ++_curr;
                return ret;
            }

            const PrincipalName& get() const { return (*_curr)->getName(); }

            const PrincipalName& operator*() const { return get(); }
            const PrincipalName* operator->() const { return &get(); }

        private:
            std::vector<Principal*>::const_iterator _curr;
            std::vector<Principal*>::const_iterator _end;
        };

        PrincipalSet();
        ~PrincipalSet();

        // If the principal is already present, this will replace the existing entry.
        // The PrincipalSet takes ownership of the passed-in principal and is responsible for
        // deleting it eventually
        void add(Principal* principal);

        // Removes all principals whose authentication credentials came from dbname.
        void removeByDBName(const StringData& dbname);

        // Returns the Principal with the given name, or NULL if not found.
        // Ownership of the returned Principal remains with the PrincipalSet.  The pointer
        // returned is only guaranteed to remain valid until the next non-const method is called
        // on the PrincipalSet.
        Principal* lookup(const PrincipalName& name) const;

        // Gets the principal whose authentication credentials came from dbname, or NULL if none
        // exist.  There should be at most one such principal.
        Principal* lookupByDBName(const StringData& dbname) const;

        // Gets an iterator over the names of the principals stored in the set.  The iterator is
        // valid until the next non-const method is called on the PrincipalSet.
        NameIterator getNames() const { return NameIterator(_principals); }

        iterator begin() const { return _principals.begin(); }
        iterator end() const { return _principals.end(); }

    private:
        // The PrincipalSet maintains ownership of the Principals in it, and is responsible for
        // deleting them when done with them.
        std::vector<Principal*> _principals;
    };

} // namespace mongo

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

#include <iosfwd>
#include <string>

#include <boost/scoped_ptr.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"

namespace mongo {

    /**
     * Representation of a name of a principal (authenticatable user) in a MongoDB system.
     *
     * Consists of a "user name" part, and a "database name" part.
     */
    class UserName {
    public:
        UserName() : _splitPoint(0) {}
        UserName(const StringData& user, const StringData& dbname);

        /**
         * Gets the user part of a UserName.
         */
        StringData getUser() const { return StringData(_fullName).substr(0, _splitPoint); }

        /**
         * Gets the database name part of a UserName.
         */
        StringData getDB() const { return StringData(_fullName).substr(_splitPoint + 1); }

        /**
         * Gets the full unique name of a user as a string, formatted as "user@db".
         */
        const std::string& getFullName() const { return _fullName; }

        /**
         * Stringifies the object, for logging/debugging.
         */
        std::string toString() const { return getFullName(); }

    private:
        std::string _fullName;  // The full name, stored as a string.  "user@db".
        size_t _splitPoint;  // The index of the "@" separating the user and db name parts.
    };

    static inline bool operator==(const UserName& lhs, const UserName& rhs) {
        return lhs.getFullName() == rhs.getFullName();
    }

    static inline bool operator!=(const UserName& lhs, const UserName& rhs) {
        return lhs.getFullName() != rhs.getFullName();
    }

    static inline bool operator<(const UserName& lhs, const UserName& rhs) {
        return lhs.getFullName() < rhs.getFullName();
    }

    std::ostream& operator<<(std::ostream& os, const UserName& name);

    /**
     * Iterator over an unspecified container of UserName objects.
     */
    class UserNameIterator {
    public:
        class Impl {
            MONGO_DISALLOW_COPYING(Impl);
        public:
            Impl() {};
            virtual ~Impl() {};
            static Impl* clone(Impl* orig) { return orig ? orig->doClone(): NULL; }
            virtual bool more() const = 0;
            virtual const UserName& get() const = 0;

            virtual const UserName& next() = 0;

        private:
            virtual Impl* doClone() const = 0;
        };

        UserNameIterator() : _impl(NULL) {}
        UserNameIterator(const UserNameIterator& other) : _impl(Impl::clone(other._impl.get())) {}
        explicit UserNameIterator(Impl* impl) : _impl(impl) {}

        UserNameIterator& operator=(const UserNameIterator& other) {
            _impl.reset(Impl::clone(other._impl.get()));
            return *this;
        }

        bool more() const { return _impl.get() && _impl->more(); }
        const UserName& get() const { return _impl->get(); }

        const UserName& next() { return _impl->next(); }

        const UserName& operator*() const { return get(); }
        const UserName* operator->() const { return &get(); }

    private:
        boost::scoped_ptr<Impl> _impl;
    };


    template <typename ContainerIterator>
    class UserNameContainerIteratorImpl : public UserNameIterator::Impl {
        MONGO_DISALLOW_COPYING(UserNameContainerIteratorImpl);
    public:
        UserNameContainerIteratorImpl(const ContainerIterator& begin,
                                      const ContainerIterator& end) :
            _curr(begin), _end(end) {}
        virtual ~UserNameContainerIteratorImpl() {}
        virtual bool more() const { return _curr != _end; }
        virtual const UserName& next() { return *(_curr++); }
        virtual const UserName& get() const { return *_curr; }
        virtual UserNameIterator::Impl* doClone() const {
            return new UserNameContainerIteratorImpl(_curr, _end);
        }

    private:
        ContainerIterator _curr;
        ContainerIterator _end;
    };

    template <typename ContainerIterator>
    UserNameIterator makeUserNameIterator(const ContainerIterator& begin,
                                          const ContainerIterator& end) {
        return UserNameIterator( new UserNameContainerIteratorImpl<ContainerIterator>(begin, end));
    }

    template <typename Container>
    UserNameIterator makeUserNameIteratorForContainer(const Container& container) {
        return makeUserNameIterator(container.begin(), container.end());
    }

}  // namespace mongo

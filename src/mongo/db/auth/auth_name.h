/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include <boost/optional.hpp>
#include <iosfwd>
#include <memory>
#include <string>

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/variant.h"

namespace mongo {

/**
 * Representation of a generic authentication name such as a UserName or RoleName.
 *
 * Consists of a general "name" part, and a "database name" part.
 */
template <typename T>
class AuthName {
public:
    AuthName() = default;

    template <typename Name, typename DB>
    AuthName(Name name, DB db, boost::optional<TenantId> tenant = boost::none) {
        if constexpr (std::is_same_v<Name, std::string>) {
            _name = std::move(name);
        } else {
            _name = StringData(name).toString();
        }

        if constexpr (std::is_same_v<DB, std::string>) {
            _db = std::move(db);
        } else {
            _db = StringData(db).toString();
        }

        _tenant = std::move(tenant);
    }

    template <typename Name>
    AuthName(Name name, const DatabaseName& dbname)
        : AuthName(name, dbname.db(), dbname.tenantId()) {}

    /**
     * Parses a string of the form "db.name" into an AuthName object with an optional tenant.
     */
    static StatusWith<T> parse(StringData str,
                               const boost::optional<TenantId>& tenant = boost::none);

    /**
     * These methods support parsing usernames from IDL
     */
    static T parseFromVariant(const stdx::variant<std::string, mongo::BSONObj>& name,
                              const boost::optional<TenantId>& tenant = boost::none);
    static T parseFromBSONObj(const BSONObj& obj,
                              const boost::optional<TenantId>& tenant = boost::none);
    static T parseFromBSON(const BSONElement& elem,
                           const boost::optional<TenantId>& tenant = boost::none);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;
    void serializeToBSON(BSONArrayBuilder* bob) const;
    void appendToBSON(BSONObjBuilder* bob, bool encodeTenant = false) const;
    BSONObj toBSON(bool encodeTenant = false) const;

    std::size_t getBSONObjSize() const;

    /**
     * Gets the name part of a AuthName.
     */
    const std::string& getName() const {
        return _name;
    }

    /**
     * Gets the database name part of an AuthName.
     */
    const std::string& getDB() const {
        return _db;
    }

    DatabaseName getDatabaseName() const {
        return DatabaseName::createDatabaseNameForAuth(_tenant, _db);
    }

    /**
     * Gets the TenantId, if any, associated with this AuthName.
     */
    const boost::optional<TenantId>& getTenant() const {
        return _tenant;
    }

    /**
     * Gets the full unique name of a user as a string, formatted as "name@db".
     */
    std::string getDisplayName() const {
        if (empty()) {
            return "";
        }
        return str::stream() << _name << "@" << _db;
    }

    /**
     * Predict name length without actually forming string.
     */
    std::size_t getDisplayNameLength() const {
        if (empty()) {
            return 0;
        }
        return _db.size() + 1 + _name.size();
    }

    /**
     * Gets the full unambiguous unique name of a user as a string, formatted as "db.name"
     */
    std::string getUnambiguousName() const {
        if (empty()) {
            return "";
        }
        return str::stream() << _db << "." << _name;
    }

    /**
     * True if the username, dbname, and tenant have not been set.
     */
    bool empty() const {
        return _db.empty() && _name.empty() && !_tenant;
    }

    bool operator==(const AuthName& rhs) const {
        return (_name == rhs._name) && (_db == rhs._db) && (_tenant == rhs._tenant);
    }

    bool operator!=(const AuthName& rhs) const {
        return !(*this == rhs);
    }

    bool operator<(const AuthName& rhs) const {
        if (_tenant != rhs._tenant) {
            return _tenant < rhs._tenant;
        } else if (_db != rhs._db) {
            return _db < rhs._db;
        } else {
            return _name < rhs._name;
        }
    }

    template <typename H>
    friend H AbslHashValue(H h, const AuthName& name) {
        auto state = std::move(h);
        if (name._tenant) {
            state = H::combine(std::move(state), TenantId::Hasher()(name._tenant.get()), '_');
        }
        return H::combine(std::move(state), name._db, '.', name._name);
    }

private:
    std::string _name;
    std::string _db;
    boost::optional<TenantId> _tenant;
};

template <typename Stream, typename T>
static inline Stream& operator<<(Stream& os, const AuthName<T>& name) {
    if (!name.empty()) {
        os << name.getName() << '@' << name.getDB();
    }
    return os;
}

/**
 * Iterator over an unspecified container of AuthName objects.
 */
template <typename T>
class AuthNameIterator {
public:
    class Impl {
    public:
        Impl() = default;
        virtual ~Impl(){};
        std::unique_ptr<Impl> clone() const {
            return std::unique_ptr<Impl>(doClone());
        }
        virtual bool more() const = 0;
        virtual const T& get() const = 0;

        virtual const T& next() = 0;

    private:
        virtual Impl* doClone() const = 0;
    };

    AuthNameIterator() = default;
    explicit AuthNameIterator(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

    bool more() const {
        return _impl.get() && _impl->more();
    }
    const T& get() const {
        return _impl->get();
    }

    const T& next() {
        return _impl->next();
    }

    const T& operator*() const {
        return get();
    }
    const T* operator->() const {
        return &get();
    }

private:
    clonable_ptr<Impl> _impl;
};


template <typename ContainerIterator, typename T>
class AuthNameContainerIteratorImpl : public AuthNameIterator<T>::Impl {
public:
    AuthNameContainerIteratorImpl(const ContainerIterator& begin, const ContainerIterator& end)
        : _curr(begin), _end(end) {}
    ~AuthNameContainerIteratorImpl() override {}
    bool more() const override {
        return _curr != _end;
    }
    const T& next() override {
        return *(_curr++);
    }
    const T& get() const override {
        return *_curr;
    }
    typename AuthNameIterator<T>::Impl* doClone() const override {
        return new AuthNameContainerIteratorImpl(*this);
    }

private:
    ContainerIterator _curr;
    ContainerIterator _end;
};

}  // namespace mongo

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

#pragma once

#include <compare>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class ClientAPIVersionParameters;
class DBClientBase;
class MongoURI;
struct TransientSSLParams;

/**
 * ConnectionString handles parsing different ways to connect to mongo and determining method
 * samples:
 *    server
 *    server:port
 *    foo/server:port,server:port   kReplicaSet
 *
 * Typical use:
 *
 * ConnectionString cs(uassertStatusOK(ConnectionString::parse(url)));
 * std::string errmsg;
 * DBClientBase * conn = cs.connect( errmsg );
 */
class ConnectionString {
public:
    enum class ConnectionType { kInvalid = 0, kStandalone, kReplicaSet, kCustom, kLocal };

    ConnectionString() = default;

    /**
     * Constructs a connection string representing a replica set.
     */
    static ConnectionString forReplicaSet(StringData replicaSetName,
                                          std::vector<HostAndPort> servers);

    /**
     * Constructs a connection string representing a list of standalone servers.
     */
    static ConnectionString forStandalones(std::vector<HostAndPort> servers);

    /**
     * Constructs a local connection string.
     */
    static ConnectionString forLocal();

    /**
     * Creates a standalone connection string with the specified server.
     */
    explicit ConnectionString(const HostAndPort& server);

    /**
     * Creates a connection string from an unparsed list of servers, type, and replicaSetName.
     */
    ConnectionString(ConnectionType type, const std::string& s, const std::string& replicaSetName);

    /**
     * Creates a connection string from a pre-parsed list of servers, type, and replicaSetName.
     */
    ConnectionString(ConnectionType type,
                     std::vector<HostAndPort> servers,
                     const std::string& replicaSetName);

    ConnectionString(const std::string& s, ConnectionType connType);

    bool isValid() const {
        return _type != ConnectionType::kInvalid;
    }

    explicit operator bool() const {
        return isValid();
    }

    const std::string& toString() const {
        return _string;
    }

    const std::string& getReplicaSetName() const {
        return _replicaSetName;
    }

    const std::string& getSetName() const {
        return getReplicaSetName();
    }

    const std::vector<HostAndPort>& getServers() const {
        return _servers;
    }

    ConnectionType type() const {
        return _type;
    }

    /**
     * Creates a new ConnectionString object which contains all the servers in either this
     * ConnectionString or the given one.  Useful for "extending" a connection string with
     * (potentially) new servers.
     *
     * The given ConnectionString must have the same type() and getSetName() as this one.
     */
    ConnectionString makeUnionWith(const ConnectionString& other);

    /**
     * Returns true if two connection strings match in terms of their type and the exact order of
     * their hosts.
     */
    bool operator==(const ConnectionString& other) const;
    bool operator!=(const ConnectionString& other) const;

    StatusWith<std::unique_ptr<DBClientBase>> connect(
        StringData applicationName,
        double socketTimeout = 0,
        const MongoURI* uri = nullptr,
        const ClientAPIVersionParameters* apiParameters = nullptr,
        const TransientSSLParams* transientSSLParams = nullptr) const;

    static StatusWith<ConnectionString> parse(const std::string& url);

    /**
     * Deserialize a ConnectionString object from a string. Used by the IDL parser for the
     * connectionstring type. Essentially just a throwing wrapper around ConnectionString::parse.
     */
    static ConnectionString deserialize(StringData url);

    static std::string typeToString(ConnectionType type);

    //
    // Allow overriding the default connection behavior
    // This is needed for some tests, which otherwise would fail because they are unable to contact
    // the correct servers.
    //

    class ConnectionHook {
    public:
        virtual ~ConnectionHook() {}

        // Returns an alternative connection object for a string
        virtual std::unique_ptr<DBClientBase> connect(
            const ConnectionString& c,
            std::string& errmsg,
            double socketTimeout,
            const ClientAPIVersionParameters* apiParameters = nullptr) = 0;
    };

    static void setConnectionHook(ConnectionHook* hook) {
        stdx::lock_guard<Latch> lk(_connectHookMutex);
        _connectHook = hook;
    }

    static ConnectionHook* getConnectionHook() {
        stdx::lock_guard<Latch> lk(_connectHookMutex);
        return _connectHook;
    }

    // Allows ConnectionStrings to be stored more easily in sets/maps
    bool operator<(const ConnectionString& other) const {
        return _string < other._string;
    }


    friend std::ostream& operator<<(std::ostream&, const ConnectionString&);
    friend StringBuilder& operator<<(StringBuilder&, const ConnectionString&);

private:
    /**
     * Creates a replica set connection string with the specified name and servers.
     */
    ConnectionString(StringData replicaSetName, std::vector<HostAndPort> servers);

    /**
     * Creates a connection string with the specified type.
     *
     * This ctor is mostly used to create ConnectionStrings to the current node with
     * ConnectionType::kLocal.
     */
    explicit ConnectionString(ConnectionType connType);

    void _fillServers(std::string s);
    void _finishInit();

    ConnectionType _type{ConnectionType::kInvalid};
    std::vector<HostAndPort> _servers;
    std::string _string;
    std::string _replicaSetName;

    static Mutex _connectHookMutex;
    static ConnectionHook* _connectHook;
};

inline std::ostream& operator<<(std::ostream& ss, const ConnectionString& cs) {
    ss << cs._string;
    return ss;
}

inline StringBuilder& operator<<(StringBuilder& sb, const ConnectionString& cs) {
    sb << cs._string;
    return sb;
}

inline std::ostream& operator<<(std::ostream& ss, const ConnectionString::ConnectionType& ct) {
    ss << ConnectionString::typeToString(ct);
    return ss;
}

inline StringBuilder& operator<<(StringBuilder& sb, const ConnectionString::ConnectionType& ct) {
    sb << ConnectionString::typeToString(ct);
    return sb;
}

}  // namespace mongo

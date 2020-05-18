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

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/platform/mutex.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * Encode a string for embedding in a URI.
 * Replaces reserved bytes with %xx sequences.
 *
 * Optionally allows passthrough characters to remain unescaped.
 */
void uriEncode(std::ostream& ss, StringData str, StringData passthrough = ""_sd);

inline std::string uriEncode(StringData str, StringData passthrough = ""_sd) {
    std::ostringstream ss;
    uriEncode(ss, str, passthrough);
    return ss.str();
}

/**
 * Decode a URI encoded string.
 * Replaces + and %xx sequences with their original byte.
 */
StatusWith<std::string> uriDecode(StringData str);

/**
 * MongoURI handles parsing of URIs for mongodb, and falls back to old-style
 * ConnectionString parsing. It's used primarily by the shell.
 * It parses URIs with the following formats:
 *
 *    mongodb://[usr:pwd@]host1[:port1]...[,hostN[:portN]]][/[db][?options]]
 *    mongodb+srv://[usr:pwd@]host[/[db][?options]]
 *
 * `mongodb+srv://` URIs will perform DNS SRV and TXT lookups and expand per the DNS Seedlist
 * specification.
 *
 * While this format is generally RFC 3986 compliant, some exceptions do exist:
 *   1. The 'host' field, as defined by section 3.2.2 is expanded in the following ways:
 *     a. Multiple hosts may be specified as a comma separated list.
 *     b. Hosts may take the form of absolute paths for unix domain sockets.
 *       i. Sockets must end in the suffix '.sock'
 *   2. The 'fragment' field, as defined by section 3.5 is not permitted.
 *
 * For a complete list of URI string options, see
 * https://docs.mongodb.com/manual/reference/connection-string/#connection-string-options
 *
 * Examples:
 *
 *    A replica set with three members (one running on default port 27017):
 *      string uri = mongodb://localhost,localhost:27018,localhost:27019
 *
 *    Authenticated connection to db 'bedrock' with user 'barney' and pwd 'rubble':
 *      string url = mongodb://barney:rubble@localhost/bedrock
 *
 *    Use parse() to parse the url, then validate and connect:
 *      string errmsg;
 *      ConnectionString cs = ConnectionString::parse( url, errmsg );
 *      if ( ! cs.isValid() ) throw "bad connection string: " + errmsg;
 *      DBClientBase * conn = cs.connect( errmsg );
 */
class MongoURI {
public:
    class CaseInsensitiveString {
    public:
        CaseInsensitiveString(std::string str);

        CaseInsensitiveString(StringData sd) : CaseInsensitiveString(std::string(sd)) {}
        CaseInsensitiveString(const char* str) : CaseInsensitiveString(std::string(str)) {}

        friend bool operator<(const CaseInsensitiveString& lhs, const CaseInsensitiveString& rhs) {
            return lhs._lowercase < rhs._lowercase;
        }

        friend bool operator==(const CaseInsensitiveString& lhs, const CaseInsensitiveString& rhs) {
            return lhs._lowercase == rhs._lowercase;
        }

        const std::string& original() const noexcept {
            return _original;
        }

    private:
        std::string _original;
        std::string _lowercase;
    };

    // Note that, because this map is used for DNS TXT record injection on options, there is a
    // requirement on its behavior for `insert`: insert must not replace or update existing values
    // -- this gives the desired behavior that user-specified values override TXT record specified
    // values.  `std::map` and `std::unordered_map` satisfy this requirement.  Make sure that
    // whichever map type is used provides that guarantee.
    using OptionsMap = std::map<CaseInsensitiveString, std::string>;

    static StatusWith<MongoURI> parse(StringData url);

    /*
     * Returns true if str starts with one of the uri schemes (e.g. mongodb:// or mongodb+srv://)
     */
    static bool isMongoURI(StringData str);

    /*
     * Returns a copy of the input url as a string with the password and connection options
     * removed. This may uassert or return a mal-formed string if the input is not a valid URI
     */
    static std::string redact(StringData url);

    DBClientBase* connect(StringData applicationName,
                          std::string& errmsg,
                          boost::optional<double> socketTimeoutSecs = boost::none) const;

    const std::string& getUser() const {
        return _user;
    }

    void setUser(std::string newUsername) {
        _user = std::move(newUsername);
    }

    const std::string& getPassword() const {
        return _password;
    }

    void setPassword(std::string newPassword) {
        _password = std::move(newPassword);
    }

    const OptionsMap& getOptions() const {
        return _options;
    }

    void setOptionIfNecessary(std::string uriParamKey, std::string value) {
        const auto key = _options.find(uriParamKey);
        if (key == end(_options) && !value.empty()) {
            _options[std::move(uriParamKey)] = std::move(value);
        }
    }

    boost::optional<std::string> getOption(const std::string& key) const {
        const auto optIter = _options.find(key);
        if (optIter != end(_options)) {
            return optIter->second;
        }
        return boost::none;
    }

    const std::string& getDatabase() const {
        return _database;
    }

    std::string getAuthenticationDatabase() const {
        auto authDB = _options.find("authSource");
        if (authDB != _options.end()) {
            return authDB->second;
        } else if (!_database.empty()) {
            return _database;
        } else {
            return "admin";
        }
    }

    bool isValid() const {
        return _connectString.isValid();
    }

    const ConnectionString& connectionString() const {
        return _connectString;
    }

    const std::string& toString() const {
        return _connectString.toString();
    }

    const std::string& getSetName() const {
        return _connectString.getSetName();
    }

    const std::vector<HostAndPort>& getServers() const {
        return _connectString.getServers();
    }


    const boost::optional<std::string> getAppName() const;

    std::string canonicalizeURIAsString() const;


    boost::optional<bool> getRetryWrites() const {
        return _retryWrites;
    }

    transport::ConnectSSLMode getSSLMode() const {
        return _sslMode;
    }

    // If you are trying to clone a URI (including its options/auth information) for a single
    // server (say a member of a replica-set), you can pass in its HostAndPort information to
    // get a new URI with the same info, except type() will be MASTER and getServers() will
    // be the single host you pass in.
    MongoURI cloneURIForServer(HostAndPort hostAndPort, StringData applicationName) const {
        auto out = *this;
        out._connectString = ConnectionString(std::move(hostAndPort));

        if (!out.getAppName()) {
            out._options["appName"] = applicationName.toString();
        }

        return out;
    }

    ConnectionString::ConnectionType type() const {
        return _connectString.type();
    }

    explicit MongoURI(const ConnectionString& connectString) : _connectString(connectString) {}

    MongoURI() = default;

    friend std::ostream& operator<<(std::ostream&, const MongoURI&);

    friend StringBuilder& operator<<(StringBuilder&, const MongoURI&);

    boost::optional<BSONObj> makeAuthObjFromOptions(
        int maxWireVersion, const std::vector<std::string>& saslMechsForAuth) const;

private:
    MongoURI(ConnectionString connectString,
             const std::string& user,
             const std::string& password,
             const std::string& database,
             boost::optional<bool> retryWrites,
             transport::ConnectSSLMode sslMode,
             OptionsMap options)
        : _connectString(std::move(connectString)),
          _user(user),
          _password(password),
          _database(database),
          _retryWrites(std::move(retryWrites)),
          _sslMode(sslMode),
          _options(std::move(options)) {}

    static MongoURI parseImpl(StringData url);

    ConnectionString _connectString;
    std::string _user;
    std::string _password;
    std::string _database;
    boost::optional<bool> _retryWrites;
    transport::ConnectSSLMode _sslMode = transport::kGlobalSSLMode;
    OptionsMap _options;
};

inline std::ostream& operator<<(std::ostream& ss, const MongoURI& uri) {
    return ss << uri._connectString;
}

inline StringBuilder& operator<<(StringBuilder& sb, const MongoURI& uri) {
    return sb << uri._connectString;
}
}  // namespace mongo

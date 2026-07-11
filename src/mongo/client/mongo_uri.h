// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/credential.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <compare>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ClientAPIVersionParameters;

/**
 * Encode a string for embedding in a URI.
 * Replaces reserved bytes with %xx sequences.
 *
 * Optionally allows passthrough characters to remain unescaped.
 */
void uriEncode(std::ostream& ss, std::string_view str, std::string_view passthrough = {});

inline std::string uriEncode(std::string_view str, std::string_view passthrough = {}) {
    std::ostringstream ss;
    uriEncode(ss, str, passthrough);
    return ss.str();
}

/**
 * Decode a URI encoded string.
 * Replaces + and %xx sequences with their original byte.
 */
StatusWith<std::string> uriDecode(std::string_view str);

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
    inline static const std::string kDefaultTestRunnerAppName = "MongoDB Shell";

    class CaseInsensitiveString {
    public:
        CaseInsensitiveString(std::string str);

        CaseInsensitiveString(std::string_view sd) : CaseInsensitiveString(std::string(sd)) {}
        CaseInsensitiveString(const char* str) : CaseInsensitiveString(std::string(str)) {}

        friend bool operator<(const CaseInsensitiveString& lhs, const CaseInsensitiveString& rhs) {
            return lhs._lowercase < rhs._lowercase;
        }

        friend bool operator==(const CaseInsensitiveString& lhs, const CaseInsensitiveString& rhs) {
            return lhs._lowercase == rhs._lowercase;
        }

        const std::string& original() const {
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

    static StatusWith<MongoURI> parse(std::string_view url);

    /*
     * Returns true if str starts with one of the uri schemes (e.g. mongodb:// or mongodb+srv://)
     */
    static bool isMongoURI(std::string_view str);

    /*
     * Returns a copy of the input url as a string with the password and connection options
     * removed. This may uassert or return a mal-formed string if the input is not a valid URI
     */
    static std::string redact(std::string_view url);

    DBClientBase* connect(
        std::string_view applicationName,
        std::string& errmsg,
        boost::optional<double> socketTimeoutSecs = boost::none,
        const ClientAPIVersionParameters* apiParameters = nullptr,
        const boost::optional<TransientSSLParams>& transientSSLParams = boost::none,
        ErrorCodes::Error* errcode = nullptr) const;

    void setUser(std::string newUsername) {
        if (_credential) {
            _credential->username = std::move(newUsername);
        } else {
            _credential = auth::Credential{auth::AuthMechanism::kScramSha256,
                                           /* db= */ boost::none,
                                           std::move(newUsername),
                                           /* password= */ boost::none,
                                           BSONObj{}};
        }
    }

    void setPassword(std::string newPassword) {
        if (_credential) {
            _credential->password = std::move(newPassword);
        } else {
            _credential = auth::Credential{auth::AuthMechanism::kScramSha256,
                                           /* db= */ boost::none,
                                           /* username= */ boost::none,
                                           std::move(newPassword),
                                           BSONObj{}};
        }
    }

    const boost::optional<auth::Credential>& getCredential() const {
        return _credential;
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

    explicit operator bool() const {
        return isValid();
    }

    const ConnectionString& connectionString() const {
        return _connectString;
    }

    const std::string& toString() const {
        return _connectString.toString();
    }

    const std::string& getReplicaSetName() const {
        return _connectString.getSetName();
    }

    const std::string& getSetName() const {
        return getReplicaSetName();
    }

    const std::vector<HostAndPort>& getServers() const {
        return _connectString.getServers();
    }


    boost::optional<std::string> getAppName() const;

    std::string canonicalizeURIAsString() const;


    boost::optional<bool> getRetryWrites() const {
        return _retryWrites;
    }

    transport::ConnectSSLMode getSSLMode() const {
        return _sslMode;
    }

    bool isHelloOk() const {
        return _helloOk.get_value_or(false);
    }

    void setHelloOk(bool helloOk) {
        invariant(!_helloOk.has_value());
        _helloOk.emplace(helloOk);
    }

#ifdef MONGO_CONFIG_GRPC
    void setIsGRPC(bool isGRPC) {
        _gRPC = isGRPC;
    }

    bool isGRPC() const {
        return _gRPC.get_value_or(false);
    }
#endif

    // If you are trying to clone a URI (including its options/auth information) for a single
    // server (say a member of a replica-set), you can pass in its HostAndPort information to
    // get a new URI with the same info, except type() will be kStandalone and getServers() will
    // be the single host you pass in.
    MongoURI cloneURIForServer(HostAndPort hostAndPort, std::string_view applicationName) const {
        auto out = *this;
        out._connectString = ConnectionString(std::move(hostAndPort));

        if (!out.getAppName()) {
            out._options["appName"] = std::string{applicationName};
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
             boost::optional<auth::Credential> credential,
             const std::string& database,
             boost::optional<bool> retryWrites,
             transport::ConnectSSLMode sslMode,
             boost::optional<bool> helloOk,
             OptionsMap options)
        : _connectString(std::move(connectString)),
          _credential(std::move(credential)),
          _database(database),
          _retryWrites(std::move(retryWrites)),
          _sslMode(sslMode),
          _helloOk(helloOk),
          _options(std::move(options)) {}

#ifdef MONGO_CONFIG_GRPC
    MongoURI(ConnectionString connectString,
             boost::optional<auth::Credential> credential,
             const std::string& database,
             boost::optional<bool> retryWrites,
             transport::ConnectSSLMode sslMode,
             boost::optional<bool> helloOk,
             boost::optional<bool> grpc,
             OptionsMap options)
        : _connectString(std::move(connectString)),
          _credential(std::move(credential)),
          _database(database),
          _retryWrites(std::move(retryWrites)),
          _sslMode(sslMode),
          _helloOk(helloOk),
          _gRPC(grpc),
          _options(std::move(options)) {}
#endif

    static MongoURI parseImpl(std::string_view url);

    ConnectionString _connectString;
    boost::optional<auth::Credential> _credential;
    std::string _database;
    boost::optional<bool> _retryWrites;
    transport::ConnectSSLMode _sslMode = transport::kGlobalSSLMode;
    boost::optional<bool> _helloOk;
#ifdef MONGO_CONFIG_GRPC
    boost::optional<bool> _gRPC;
#endif
    OptionsMap _options;
};

inline std::ostream& operator<<(std::ostream& ss, const MongoURI& uri) {
    return ss << uri._connectString;
}

inline StringBuilder& operator<<(StringBuilder& sb, const MongoURI& uri) {
    return sb << uri._connectString;
}
}  // namespace mongo

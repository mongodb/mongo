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

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/compare.hpp>
#include <boost/algorithm/string/find_iterator.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/range/algorithm/count.hpp>
// IWYU pragma: no_include "boost/algorithm/string/detail/classification.hpp"
// IWYU pragma: no_include "boost/algorithm/string/detail/finder.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/utility.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/dns_name.h"
#include "mongo/util/dns_query.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include <boost/algorithm/string/finder.hpp>
#include <boost/core/addressof.hpp>
#include <boost/function/function_base.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/range/const_iterator.hpp>
#include <boost/range/iterator_range_core.hpp>
#include <boost/type_index/type_index_facade.hpp>

using namespace std::literals::string_literals;

namespace {
constexpr std::array<char, 16> hexits{
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

// This vector must remain sorted.  It is over pairs to facilitate a call to `std::includes` using
// a `std::map<std::string, std::string>` as the other parameter.
const std::vector<std::pair<std::string, std::string>> permittedTXTOptions = {{"authSource"s, ""s},
                                                                              {"replicaSet"s, ""s}};

}  // namespace

/**
 * RFC 3986 Section 2.1 - Percent Encoding
 *
 * Encode data elements in a way which will allow them to be embedded
 * into a mongodb:// URI safely.
 */
void mongo::uriEncode(std::ostream& ss, StringData toEncode, StringData passthrough) {
    for (const auto& c : toEncode) {
        if ((c == '-') || (c == '_') || (c == '.') || (c == '~') || ctype::isAlnum(c) ||
            (passthrough.find(c) != std::string::npos)) {
            ss << c;
        } else {
            // Encoding anything not included in section 2.3 "Unreserved characters"
            ss << '%' << hexits[(c >> 4) & 0xF] << hexits[c & 0xF];
        }
    }
}

mongo::StatusWith<std::string> mongo::uriDecode(StringData toDecode) {
    StringBuilder out;
    for (size_t i = 0; i < toDecode.size(); ++i) {
        char c = toDecode[i];
        if (c == '%') {
            if (i + 2 >= toDecode.size()) {
                return Status(ErrorCodes::FailedToParse,
                              "Encountered partial escape sequence at end of string");
            }
            try {
                c = hexblob::decodePair(toDecode.substr(i + 1, 2));
            } catch (const ExceptionFor<ErrorCodes::FailedToParse>&) {
                return Status(ErrorCodes::Error(51040),
                              "The characters after the % do not form a hex value. Please escape "
                              "the % or pass a valid hex value. ");
            }
            i += 2;
        }
        out << c;
    }
    return out.str();
}

namespace mongo {

namespace {

constexpr StringData kURIPrefix = "mongodb://"_sd;
constexpr StringData kURISRVPrefix = "mongodb+srv://"_sd;
constexpr StringData kDefaultMongoHost = "127.0.0.1:27017"_sd;

/**
 * Helper Method for MongoURI::parse() to split a string into exactly 2 pieces by a char
 * delimiter.
 */
std::pair<StringData, StringData> partitionForward(StringData str, const char c) {
    const auto delim = str.find(c);
    if (delim == std::string::npos) {
        return {str, StringData()};
    }
    return {str.substr(0, delim), str.substr(delim + 1)};
}

/**
 * Helper method for MongoURI::parse() to split a string into exactly 2 pieces by a char
 * delimiter searching backward from the end of the string.
 */
std::pair<StringData, StringData> partitionBackward(StringData str, const char c) {
    const auto delim = str.rfind(c);
    if (delim == std::string::npos) {
        return {StringData(), str};
    }
    return {str.substr(0, delim), str.substr(delim + 1)};
}

/**
 * Breakout method for parsing application/x-www-form-urlencoded option pairs
 *
 * foo=bar&baz=qux&...
 *
 * A `std::map<std::string, std::string>` is returned, to facilitate setwise operations from the STL
 * on multiple parsed option sources.  STL setwise operations require sorted lists.  A map is used
 * instead of a vector of pairs to permit insertion-is-not-overwrite behavior.
 */
MongoURI::OptionsMap parseOptions(StringData options, StringData url) {
    MongoURI::OptionsMap ret;
    if (options.empty()) {
        return ret;
    }

    if (options.find('?') != std::string::npos) {
        uasserted(
            ErrorCodes::FailedToParse,
            str::stream() << "URI Cannot Contain multiple questions marks for mongodb:// URL: "
                          << url);
    }

    const auto optionsStr = std::string{options};
    for (auto i =
             boost::make_split_iterator(optionsStr, boost::first_finder("&", boost::is_iequal()));
         i != std::remove_reference<decltype((i))>::type{};
         ++i) {
        const auto opt = boost::copy_range<std::string>(*i);
        if (opt.empty()) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Missing a key/value pair in the options for mongodb:// URL: " << url);
        }

        const auto kvPair = partitionForward(opt, '=');
        const auto keyRaw = kvPair.first;
        if (keyRaw.empty()) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Missing a key for key/value pair in the options for mongodb:// URL: "
                          << url);
        }
        const auto key = uassertStatusOKWithContext(
            uriDecode(keyRaw),
            str::stream() << "Key '" << keyRaw
                          << "' in options cannot properly be URL decoded for mongodb:// URL: "
                          << url);
        const auto valRaw = kvPair.second;
        if (valRaw.empty()) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Missing value for key '" << keyRaw
                                    << "' in the options for mongodb:// URL: " << url);
        }
        const auto val = uassertStatusOKWithContext(
            uriDecode(valRaw),
            str::stream() << "Value '" << valRaw << "' for key '" << keyRaw
                          << "' in options cannot properly be URL decoded for mongodb:// URL: "
                          << url);

        ret[key] = val;
    }

    return ret;
}

MongoURI::OptionsMap addTXTOptions(MongoURI::OptionsMap options,
                                   const std::string& host,
                                   const StringData url,
                                   const bool isSeedlist) {
    // If there is no seedlist mode, then don't add any TXT options.
    if (!isSeedlist)
        return options;
    options.insert({"ssl", "true"});

    // Get all TXT records and parse them as options, adding them to the options set.
    auto txtRecords = dns::getTXTRecords(host);
    if (txtRecords.empty()) {
        return {std::make_move_iterator(begin(options)), std::make_move_iterator(end(options))};
    }

    if (txtRecords.size() > 1) {
        uasserted(ErrorCodes::FailedToParse, "Encountered multiple TXT records for: "s + url);
    }

    auto txtOptions = parseOptions(txtRecords.front(), url);
    if (!std::includes(
            begin(permittedTXTOptions),
            end(permittedTXTOptions),
            begin(stdx::as_const(txtOptions)),
            end(stdx::as_const(txtOptions)),
            [](const auto& lhs, const auto& rhs) { return std::get<0>(lhs) < std::get<0>(rhs); })) {
        uasserted(ErrorCodes::FailedToParse, "Encountered invalid options in TXT record.");
    }

    options.insert(std::make_move_iterator(begin(txtOptions)),
                   std::make_move_iterator(end(txtOptions)));

    return {std::make_move_iterator(begin(options)), std::make_move_iterator(end(options))};
}

// Contains the parts of a MongoURI as unowned StringData's. Any code that needs to break up
// URIs into their basic components without fully parsing them can use this struct.
// Internally, MongoURI uses this to do basic parsing of the input URI string.
struct URIParts {
    explicit URIParts(StringData uri);
    StringData scheme;
    StringData username;
    StringData password;
    StringData hostIdentifiers;
    StringData database;
    StringData options;
};

URIParts::URIParts(StringData uri) {
    // 1. Strip off the scheme ("mongo://")
    auto schemeEnd = uri.find("://");
    if (schemeEnd == std::string::npos) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "URI must begin with " << kURIPrefix << " or " << kURISRVPrefix
                                << ": " << uri);
    }
    const auto uriWithoutPrefix = uri.substr(schemeEnd + 3);
    scheme = uri.substr(0, schemeEnd);

    // 2. Split the string by the first, unescaped / (if any), yielding:
    // split[0]: User information and host identifers
    // split[1]: Auth database and connection options
    const auto userAndDb = partitionForward(uriWithoutPrefix, '/');
    const auto userAndHostInfo = userAndDb.first;

    // 2.b Make sure that there are no question marks in the left side of the /
    //     as any options after the ? must still have the / delimiter
    if (userAndDb.second.empty() && userAndHostInfo.find('?') != std::string::npos) {
        uasserted(
            ErrorCodes::FailedToParse,
            str::stream()
                << "URI must contain slash delimiter between hosts and options for mongodb:// URL: "
                << uri);
    }

    // 3. Split the user information and host identifiers string by the last, unescaped @,
    const auto userAndHost = partitionBackward(userAndHostInfo, '@');
    const auto userInfo = userAndHost.first;
    hostIdentifiers = userAndHost.second;

    // 4. Split up the username and password
    const auto userAndPass = partitionForward(userInfo, ':');
    username = userAndPass.first;
    password = userAndPass.second;

    // 5. Split the database name from the list of options
    const auto databaseAndOptions = partitionForward(userAndDb.second, '?');
    database = databaseAndOptions.first;
    options = databaseAndOptions.second;
}
}  // namespace

MongoURI::CaseInsensitiveString::CaseInsensitiveString(std::string str)
    : _original(std::move(str)), _lowercase(boost::algorithm::to_lower_copy(_original)) {}

bool MongoURI::isMongoURI(StringData uri) {
    return (uri.starts_with(kURIPrefix) || uri.starts_with(kURISRVPrefix));
}

std::string MongoURI::redact(StringData url) {
    uassert(50892, "String passed to MongoURI::redact wasn't a MongoURI", isMongoURI(url));
    URIParts parts(url);
    std::ostringstream out;

    out << parts.scheme << "://";
    if (!parts.username.empty()) {
        out << parts.username << "@";
    }
    out << parts.hostIdentifiers;
    if (!parts.database.empty()) {
        out << "/" << parts.database;
    }

    return out.str();
}

MongoURI MongoURI::parseImpl(StringData url) {
    // 1. Validate and remove the scheme prefix `mongodb://` or `mongodb+srv://`
    const bool isSeedlist = url.starts_with(kURISRVPrefix);
    if (!(url.starts_with(kURIPrefix) || isSeedlist)) {
        return MongoURI(uassertStatusOK(ConnectionString::parse(std::string{url})));
    }

    // 2. Split up the URI into its components for further parsing and validation
    URIParts parts(url);
    const auto hostIdentifiers = parts.hostIdentifiers;
    const auto usernameSD = parts.username;
    const auto passwordSD = parts.password;
    const auto databaseSD = parts.database;
    const auto connectionOptions = parts.options;

    // 3. URI decode and validate the username/password
    const auto containsColonOrAt = [](StringData str) {
        return (str.find(':') != std::string::npos) || (str.find('@') != std::string::npos);
    };

    if (containsColonOrAt(usernameSD)) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Username must be URL Encoded for mongodb:// URL: " << url);
    }

    if (containsColonOrAt(passwordSD)) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Password must be URL Encoded for mongodb:// URL: " << url);
    }

    // Get the username and make sure it did not fail to decode
    const auto username = uassertStatusOKWithContext(
        uriDecode(usernameSD),
        str::stream() << "Username cannot properly be URL decoded for mongodb:// URL: " << url);

    // Get the password and make sure it did not fail to decode
    const auto password = uassertStatusOKWithContext(
        uriDecode(passwordSD),
        str::stream() << "Password cannot properly be URL decoded for mongodb:// URL: " << url);

    // 4. Validate, split, and URL decode the host identifiers.
    const auto hostIdentifiersStr = std::string{hostIdentifiers};
    std::vector<HostAndPort> servers;
    for (auto i = boost::make_split_iterator(hostIdentifiersStr,
                                             boost::first_finder(",", boost::is_iequal()));
         i != std::remove_reference<decltype((i))>::type{};
         ++i) {
        const auto host = uassertStatusOKWithContext(
            uriDecode(boost::copy_range<std::string>(*i)),
            str::stream() << "Host cannot properly be URL decoded for mongodb:// URL: " << url);

        if (host.empty()) {
            continue;
        }

        if ((host.find('/') != std::string::npos) && !StringData(host).ends_with(".sock")) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "'" << host << "' in '" << url
                          << "' appears to be a unix socket, but does not end in '.sock'");
        }

        servers.push_back(uassertStatusOK(HostAndPort::parse(host)));
    }
    if (servers.empty()) {
        uasserted(ErrorCodes::FailedToParse, "No server(s) specified");
    }

    const std::string canonicalHost = servers.front().host();
    // If we're in seedlist mode, lookup the SRV record for `_mongodb._tcp` on the specified
    // domain name.  Take that list of servers as the new list of servers.
    if (isSeedlist) {
        if (servers.size() > 1) {
            uasserted(ErrorCodes::FailedToParse,
                      "Only a single server may be specified with a mongo+srv:// url.");
        }

        const mongo::dns::HostName host(canonicalHost);

        if (host.nameComponents().size() < 3) {
            uasserted(ErrorCodes::FailedToParse,
                      "A server specified with a mongo+srv:// url must have at least 3 hostname "
                      "components separated by dots ('.')");
        }

        const mongo::dns::HostName srvSubdomain("_mongodb._tcp");

        const auto srvEntries =
            dns::lookupSRVRecords(srvSubdomain.resolvedIn(host).canonicalName());

        auto makeFQDN = [](dns::HostName hostName) {
            hostName.forceQualification();
            return hostName;
        };

        const mongo::dns::HostName domain = makeFQDN(host.parentDomain());
        servers.clear();
        using std::begin;
        using std::end;
        std::transform(
            begin(srvEntries), end(srvEntries), back_inserter(servers), [&domain](auto&& srv) {
                const dns::HostName target(srv.first.host);  // FQDN

                if (!domain.contains(target)) {
                    uasserted(ErrorCodes::FailedToParse,
                              str::stream() << "Hostname " << target << " is not within the domain "
                                            << domain);
                }
                return HostAndPort(target.noncanonicalName(), srv.first.port);
            });
    }

    // 5. Decode the database name
    const auto database =
        uassertStatusOKWithContext(uriDecode(databaseSD),
                                   str::stream() << "Database name cannot properly be URL "
                                                    "decoded for mongodb:// URL: "
                                                 << url);

    // 6. Validate the database contains no prohibited characters
    // Prohibited characters:
    // slash ("/"), backslash ("\"), space (" "), double-quote ("""), or dollar sign ("$")
    // period (".") is also prohibited, but drivers MAY allow periods
    if (!database.empty() &&
        !DatabaseName::validDBName(database, DatabaseName::DollarInDbNameBehavior::Disallow)) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Database name cannot have reserved "
                                   "characters for mongodb:// URL: "
                                << url);
    }

    // 7. Validate, split, and URL decode the connection options
    auto options =
        addTXTOptions(parseOptions(connectionOptions, url), canonicalHost, url, isSeedlist);

    // If a replica set option was specified, store it in the 'replicaSetName' field.
    auto optIter = options.find("replicaSet");
    std::string replicaSetName;
    if (optIter != end(options)) {
        replicaSetName = optIter->second;
        invariant(!replicaSetName.empty());
    }

    // If an appName option was specified, validate that is 128 bytes or less.
    optIter = options.find("appName");
    if (optIter != end(options) && optIter->second.length() > 128) {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "appName cannot exceed 128 characters: " << optIter->second);
    }

    auto extractBooleanOption =
        [&options](CaseInsensitiveString optionName) -> boost::optional<bool> {
        auto optIter = options.find(optionName);
        if (optIter == end(options)) {
            return boost::none;
        }
        if (auto value = optIter->second; value == "true") {
            return true;
        } else if (value == "false") {
            return false;
        }
        uasserted(ErrorCodes::FailedToParse,
                  fmt::format("{} must be either \"true\" or \"false\"", optionName.original()));
    };

    const auto retryWrites = extractBooleanOption("retryWrites");
    const auto helloOk = extractBooleanOption("helloOk");
#ifdef MONGO_CONFIG_GRPC
    const auto gRPC = extractBooleanOption("gRPC");
#endif
    auto tlsEnabled = extractBooleanOption("tls");
    if (!tlsEnabled.has_value()) {
        tlsEnabled = extractBooleanOption("ssl");
    }

    auto tlsMode = transport::ConnectSSLMode::kGlobalSSLMode;
    if (tlsEnabled.has_value()) {
        tlsMode = *tlsEnabled ? transport::ConnectSSLMode::kEnableSSL
                              : transport::ConnectSSLMode::kDisableSSL;
    }

    auto cs = replicaSetName.empty()
        ? ConnectionString::forStandalones(std::move(servers))
        : ConnectionString::forReplicaSet(replicaSetName, std::move(servers));
    return MongoURI(std::move(cs),
                    username,
                    password,
                    database,
                    retryWrites,
                    tlsMode,
                    helloOk,
#ifdef MONGO_CONFIG_GRPC
                    gRPC,
#endif
                    std::move(options));
}

StatusWith<MongoURI> MongoURI::parse(StringData url) try {
    return parseImpl(url);
} catch (const std::exception&) {
    return exceptionToStatus();
}

boost::optional<std::string> MongoURI::getAppName() const {
    const auto optIter = _options.find("appName");
    if (optIter != end(_options)) {
        return optIter->second;
    }
    return boost::none;
}

std::string MongoURI::canonicalizeURIAsString() const {
    StringBuilder uri;
    uri << kURIPrefix;
    if (!_user.empty()) {
        uri << uriEncode(_user);
        if (!_password.empty()) {
            uri << ":" << uriEncode(_password);
        }
        uri << "@";
    }

    const auto& servers = _connectString.getServers();
    if (!servers.empty()) {
        auto delimeter = "";
        for (auto& hostAndPort : servers) {
            if (boost::count(hostAndPort.host(), ':') > 1) {
                uri << delimeter << "[" << uriEncode(hostAndPort.host()) << "]"
                    << ":" << uriEncode(std::to_string(hostAndPort.port()));
            } else if (StringData(hostAndPort.host()).ends_with(".sock")) {
                uri << delimeter << uriEncode(hostAndPort.host());
            } else {
                uri << delimeter << uriEncode(hostAndPort.host()) << ":"
                    << uriEncode(std::to_string(hostAndPort.port()));
            }
            delimeter = ",";
        }
    } else {
        uri << kDefaultMongoHost;
    }

    uri << "/";
    if (!_database.empty()) {
        uri << uriEncode(_database);
    }

    if (!_options.empty()) {
        auto delimeter = "";
        uri << "?";
        for (const auto& pair : _options) {
            uri << delimeter << uriEncode(pair.first.original()) << "=" << uriEncode(pair.second);
            delimeter = "&";
        }
    }
    return uri.str();
}

namespace {
constexpr auto kAuthMechanismPropertiesKey = "mechanism_properties"_sd;

constexpr auto kAuthServiceName = "SERVICE_NAME"_sd;
constexpr auto kAuthServiceRealm = "SERVICE_REALM"_sd;
constexpr auto kAuthAwsSessionToken = "AWS_SESSION_TOKEN"_sd;
constexpr auto kAuthOIDCAccessToken = "OIDC_ACCESS_TOKEN"_sd;

constexpr std::array<StringData, 4> kSupportedAuthMechanismProperties = {
    kAuthServiceName, kAuthServiceRealm, kAuthAwsSessionToken, kAuthOIDCAccessToken};

BSONObj parseAuthMechanismProperties(const std::string& propStr) {
    BSONObjBuilder bob;
    std::vector<std::string> props;
    boost::algorithm::split(props, propStr, boost::algorithm::is_any_of(",:"));
    for (std::vector<std::string>::const_iterator it = props.begin(); it != props.end(); ++it) {
        std::string prop((boost::algorithm::to_upper_copy(*it)));  // normalize case
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "authMechanismProperty: " << *it << " is not supported",
                std::count(std::begin(kSupportedAuthMechanismProperties),
                           std::end(kSupportedAuthMechanismProperties),
                           StringData(prop)));
        ++it;
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "authMechanismProperty: " << prop << " must have a value",
                it != props.end());
        bob.append(prop, *it);
    }
    return bob.obj();
}
}  // namespace

boost::optional<BSONObj> MongoURI::makeAuthObjFromOptions(
    int maxWireVersion, const std::vector<std::string>& saslMechsForAuth) const {
    // Usually, a username is required to authenticate.
    // However certain authentication mechanisms may omit the username.
    // This includes X509, which infers the username from the certificate;
    // AWS-IAM, which infers it from the session token;
    // and OIDC, which infers it from the access token.
    bool usernameRequired = true;

    BSONObjBuilder bob;
    if (!_password.empty()) {
        bob.append(saslCommandPasswordFieldName, _password);
    }

    auto it = _options.find("authSource");
    if (it != _options.end()) {
        bob.append(saslCommandUserDBFieldName, it->second);
    } else if (!_database.empty()) {
        bob.append(saslCommandUserDBFieldName, _database);
    } else {
        bob.append(saslCommandUserDBFieldName, "admin");
    }

    it = _options.find("authMechanism");
    if (it != _options.end()) {
        bob.append(saslCommandMechanismFieldName, it->second);
        if (it->second == auth::kMechanismMongoX509 || it->second == auth::kMechanismMongoAWS ||
            it->second == auth::kMechanismMongoOIDC) {
            usernameRequired = false;
        }
    } else if (std::find(saslMechsForAuth.begin(),
                         saslMechsForAuth.end(),
                         auth::kMechanismScramSha256) != saslMechsForAuth.end()) {
        bob.append(saslCommandMechanismFieldName, auth::kMechanismScramSha256);
    } else {
        bob.append(saslCommandMechanismFieldName, auth::kMechanismScramSha1);
    }

    if (usernameRequired && _user.empty()) {
        return boost::none;
    }

    std::string username(_user);  // may have to tack on service realm before we append

    it = _options.find("authMechanismProperties");
    if (it != _options.end()) {
        BSONObj parsed(parseAuthMechanismProperties(it->second));

        bool hasNameProp = parsed.hasField(kAuthServiceName);
        bool hasRealmProp = parsed.hasField(kAuthServiceRealm);

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both gssapiServiceName and SERVICE_NAME",
                !(hasNameProp && _options.count("gssapiServiceName")));
        // we append the parsed object so that mechanisms that don't accept it can assert.
        bob.append(kAuthMechanismPropertiesKey, parsed);
        // we still append using the old way the SASL code expects it
        if (hasNameProp) {
            bob.append(saslCommandServiceNameFieldName, parsed[kAuthServiceName].String());
        }
        // if we specified a realm, we just append it to the username as the SASL code
        // expects it that way.
        if (hasRealmProp) {
            if (username.empty()) {
                // In practice, this won't actually occur since
                // this block corresponds to GSSAPI, while username
                // may only be omitted with MONGODB-X509.
                return boost::none;
            }
            username.append("@").append(parsed[kAuthServiceRealm].String());
        }

        if (parsed.hasField(kAuthAwsSessionToken)) {
            bob.append(saslCommandIamSessionToken, parsed[kAuthAwsSessionToken].String());
        }

        if (parsed.hasField(kAuthOIDCAccessToken)) {
            bob.append(saslCommandOIDCAccessToken, parsed[kAuthOIDCAccessToken].String());
        }
    }

    it = _options.find("gssapiServiceName");
    if (it != _options.end()) {
        bob.append(saslCommandServiceNameFieldName, it->second);
    }

    if (!username.empty()) {
        bob.append("user", username);
    }

    return bob.obj();
}

}  // namespace mongo

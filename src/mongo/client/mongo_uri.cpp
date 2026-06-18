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
#include <string_view>
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
#include <fmt/format.h>

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
void mongo::uriEncode(std::ostream& ss, std::string_view toEncode, std::string_view passthrough) {
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

mongo::StatusWith<std::string> mongo::uriDecode(std::string_view toDecode) {
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
using namespace std::literals::string_view_literals;

namespace {

constexpr std::string_view kURIPrefix = "mongodb://"sv;
constexpr std::string_view kURISRVPrefix = "mongodb+srv://"sv;
constexpr std::string_view kDefaultMongoHost = "127.0.0.1:27017"sv;

/**
 * Helper Method for MongoURI::parse() to split a string into exactly 2 pieces by a char
 * delimiter.
 */
std::pair<std::string_view, std::string_view> partitionForward(std::string_view str, const char c) {
    const auto delim = str.find(c);
    if (delim == std::string::npos) {
        return {str, std::string_view()};
    }
    return {str.substr(0, delim), str.substr(delim + 1)};
}

/**
 * Helper method for MongoURI::parse() to split a string into exactly 2 pieces by a char
 * delimiter searching backward from the end of the string.
 */
std::pair<std::string_view, std::string_view> partitionBackward(std::string_view str,
                                                                const char c) {
    const auto delim = str.rfind(c);
    if (delim == std::string::npos) {
        return {std::string_view(), str};
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
MongoURI::OptionsMap parseOptions(std::string_view options, std::string_view url) {
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
                                   const std::string_view url,
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
        uasserted(ErrorCodes::FailedToParse,
                  fmt::format("Encountered multiple TXT records for: {}", url));
    }

    auto txtOptions = parseOptions(txtRecords.front(), url);
    if (!std::includes(
            begin(permittedTXTOptions),
            end(permittedTXTOptions),
            begin(std::as_const(txtOptions)),
            end(std::as_const(txtOptions)),
            [](const auto& lhs, const auto& rhs) { return std::get<0>(lhs) < std::get<0>(rhs); })) {
        uasserted(ErrorCodes::FailedToParse, "Encountered invalid options in TXT record.");
    }

    options.insert(std::make_move_iterator(begin(txtOptions)),
                   std::make_move_iterator(end(txtOptions)));

    return {std::make_move_iterator(begin(options)), std::make_move_iterator(end(options))};
}

// Contains the parts of a MongoURI as unowned std::string_view's. Any code that needs to break up
// URIs into their basic components without fully parsing them can use this struct.
// Internally, MongoURI uses this to do basic parsing of the input URI string.
struct URIParts {
    explicit URIParts(std::string_view uri);
    std::string_view scheme;
    std::string_view username;
    std::string_view password;
    std::string_view hostIdentifiers;
    std::string_view database;
    std::string_view options;
};

URIParts::URIParts(std::string_view uri) {
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

// Resolves the auth mechanism with the following precedence:
// 1. Explicit authMechanism URI option.
// 2. SCRAM mechanism advertised by the server for the selected auth source (prefer SHA-256).
// 3. Default to SCRAM-SHA-256 when server mechanisms are unavailable.
StatusWith<auth::AuthMechanism> resolveMechanism(
    const MongoURI::OptionsMap& options,
    boost::optional<std::vector<std::string>> saslMechsForAuth = boost::none) {
    if (auto it = options.find("authMechanism"); it != options.end())
        return auth::authMechanismFromString(it->second);
    if (saslMechsForAuth) {
        return std::find(saslMechsForAuth->begin(),
                         saslMechsForAuth->end(),
                         auth::kMechanismScramSha256) != saslMechsForAuth->end()
            ? auth::AuthMechanism::kScramSha256
            : auth::AuthMechanism::kScramSha1;
    }
    return auth::AuthMechanism::kScramSha256;
}

bool mechanismRequiresUsername(auth::AuthMechanism mechanism) {
    return mechanism != auth::AuthMechanism::kMongoX509 &&
        mechanism != auth::AuthMechanism::kMongoAWS && mechanism != auth::AuthMechanism::kMongoOIDC;
}

// Resolves the authentication database with the following precedence:
// 1. Explicit non-empty authSource URI option.
// 2. For X.509/AWS/OIDC/GSSAPI, default to $external.
// 3. For PLAIN, use the URI database when present, otherwise $external.
// 4. For all other mechanisms, use the URI database when present, otherwise admin.
std::string resolveAuthSource(auth::AuthMechanism mechanism,
                              const MongoURI::OptionsMap& options,
                              std::string_view database) {
    const bool isExternalDefaultDbMech = mechanism == auth::AuthMechanism::kMongoX509 ||
        mechanism == auth::AuthMechanism::kMongoAWS ||
        mechanism == auth::AuthMechanism::kMongoOIDC || mechanism == auth::AuthMechanism::kGSSAPI;
    if (auto it = options.find("authSource"); it != options.end() && !it->second.empty())
        return it->second;
    if (isExternalDefaultDbMech)
        return std::string{DatabaseName::kExternal.db(omitTenant)};
    if (mechanism == auth::AuthMechanism::kSaslPlain)
        return !database.empty() ? std::string{database}
                                 : std::string{DatabaseName::kExternal.db(omitTenant)};
    return !database.empty() ? std::string{database}
                             : std::string{DatabaseName::kAdmin.db(omitTenant)};
}

}  // namespace

MongoURI::CaseInsensitiveString::CaseInsensitiveString(std::string str)
    : _original(std::move(str)), _lowercase(boost::algorithm::to_lower_copy(_original)) {}

bool MongoURI::isMongoURI(std::string_view uri) {
    return (uri.starts_with(kURIPrefix) || uri.starts_with(kURISRVPrefix));
}

std::string MongoURI::redact(std::string_view url) {
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

MongoURI MongoURI::parseImpl(std::string_view url) {
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
    const auto containsColonOrAt = [](std::string_view str) {
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

        if ((host.find('/') != std::string::npos) && !std::string_view(host).ends_with(".sock")) {
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

    // Build a Credential if the URI carries authentication data.
    boost::optional<auth::Credential> credential;
    {
        auto swMech = resolveMechanism(options);
        uassertStatusOK(swMech);
        const auto mechanism = swMech.getValue();
        // Build credentials when either:
        // - a username is present, or
        // - the selected mechanism allows username omission (for example X509/AWS/OIDC).
        // In the latter case, a non-empty username is still valid and is forwarded when building
        // the auth command.
        if (!username.empty() || !mechanismRequiresUsername(mechanism)) {
            credential = auth::Credential{
                .mechanism = mechanism,
                .db = resolveAuthSource(mechanism, options, database),
                .username = username.empty() ? boost::none : boost::optional<std::string>{username},
                .password =
                    password.empty() ? boost::none : boost::optional<std::string>{password}};
        }
    }

    auto cs = replicaSetName.empty()
        ? ConnectionString::forStandalones(std::move(servers))
        : ConnectionString::forReplicaSet(replicaSetName, std::move(servers));
    return MongoURI(std::move(cs),
                    std::move(credential),
                    database,
                    retryWrites,
                    tlsMode,
                    helloOk,
#ifdef MONGO_CONFIG_GRPC
                    gRPC,
#endif
                    std::move(options));
}

StatusWith<MongoURI> MongoURI::parse(std::string_view url) try {
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
    if (_credential && _credential->username && !_credential->username->empty()) {
        uri << uriEncode(*_credential->username);
        if (_credential->password && !_credential->password->empty()) {
            uri << ":" << uriEncode(*_credential->password);
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
            } else if (std::string_view(hostAndPort.host()).ends_with(".sock")) {
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
constexpr auto kAuthMechanismPropertiesKey = "mechanism_properties"sv;

constexpr auto kAuthServiceName = "SERVICE_NAME"sv;
constexpr auto kAuthServiceRealm = "SERVICE_REALM"sv;
constexpr auto kAuthAwsSessionToken = "AWS_SESSION_TOKEN"sv;
constexpr auto kAuthOIDCAccessToken = "OIDC_ACCESS_TOKEN"sv;

constexpr std::array<std::string_view, 4> kSupportedAuthMechanismProperties = {
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
                           std::string_view(prop)));
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

    const auto credUser =
        (_credential && _credential->username) ? *_credential->username : std::string{};
    const auto credPassword =
        (_credential && _credential->password) ? *_credential->password : std::string{};

    BSONObjBuilder bob;
    if (!credPassword.empty()) {
        bob.append(saslCommandPasswordFieldName, credPassword);
    }

    const auto mechanism = resolveMechanism(_options, saslMechsForAuth).getValue();
    usernameRequired = mechanismRequiresUsername(mechanism);
    bob.append(saslCommandUserDBFieldName, resolveAuthSource(mechanism, _options, _database));
    bob.append(saslCommandMechanismFieldName, auth::toString(mechanism));

    if (usernameRequired && credUser.empty()) {
        return boost::none;
    }

    std::string username = credUser;  // may have to tack on service realm before we append

    auto it = _options.find("authMechanismProperties");
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

    it = _options.find("gssapiHostName");
    if (it != _options.end()) {
        bob.append(saslCommandServiceHostnameFieldName, it->second);
    }

    if (!username.empty()) {
        bob.append("user", username);
    }

    return bob.obj();
}

}  // namespace mongo

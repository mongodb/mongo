/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/mongo_uri.h"

#include <tuple>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/hex.h"
#include "mongo/util/mongoutils/str.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/find_iterator.hpp>
#include <boost/algorithm/string/predicate.hpp>

namespace mongo {

namespace {

// Helper Method for MongoURI::parse() to urlDecode the components of the URI
StatusWith<std::string> urlDecode(StringData toDecode) {
    std::ostringstream out;
    for (size_t i = 0; i < toDecode.size(); ++i) {
        if (toDecode.substr(i, 1) == "%") {
            if (i + 2 > toDecode.size()) {
                return Status(ErrorCodes::FailedToParse, "");
            }
            out << mongo::fromHex(toDecode.substr(i + 1, 2));
            i += 2;
        } else {
            out << toDecode.substr(i, 1);
        }
    }
    return out.str();
}

/*  Helper Method for MongoURI::parse() to split a string into exactly 2 pieces by a char delimeter
 */

/*  partitionForward() splits a string by the first occurance of the character delimeter

    Params:
        str: The string to be split
        c: The char delimeter

    Returns:
        std::tuple<StringData, StringData>
*/
std::tuple<StringData, StringData> partitionForward(const StringData str, const char c) {
    std::size_t delim = str.find(c);
    if (delim == std::string::npos) {
        return std::make_tuple(str, StringData());
    }
    return std::make_tuple(str.substr(0, delim), str.substr(delim + 1));
}

/*  partitionBackward() splits a string by the last occurance of the character delimeter

    Params:
        str: The string to be split
        c: The char delimeter

    Returns:
        std::tuple<StringData, StringData>
*/
std::tuple<StringData, StringData> partitionBackward(const StringData str, const char c) {
    std::size_t delim = str.rfind(c);
    if (delim == std::string::npos) {
        return std::make_tuple(StringData(), str);
    }
    return std::make_tuple(str.substr(0, delim), str.substr(delim + 1));
}


}  // namespace

StatusWith<MongoURI> MongoURI::parse(const std::string& url) {
    StringData urlSD = StringData(url);
    StringData prefix("mongodb://");
    if (!urlSD.startsWith(prefix)) {
        auto cs_status = ConnectionString::parse(url);
        if (!cs_status.isOK()) {
            return cs_status.getStatus();
        }
        return MongoURI(cs_status.getValue());
    }

    // 1. Validate and remove the scheme prefix mongodb://
    StringData uriWithoutPrefix = urlSD.substr(prefix.size());

    // 2. Split the string by the first, unescaped / (if any), yielding:
    // split[0]: User information and host identifers
    // split[1]: Auth database and connection options
    auto t1 = partitionForward(uriWithoutPrefix, '/');
    StringData userAndHostInfo = std::get<0>(t1);
    StringData databaseAndOptions = std::get<1>(t1);

    // 2.b Make sure that there are no question marks in the left side of the /
    //     as any options after the ? must still have the / delimeter
    if (databaseAndOptions.empty() &&
        userAndHostInfo.toString().find_first_of("?") != std::string::npos) {
        return Status(
            ErrorCodes::FailedToParse,
            str::stream()
                << "URI must contain slash delimeter between hosts and options for mongodb:// URL: "
                << url);
    }

    // 3. Split the user information and host identifiers string by the last, unescaped @, yielding:
    // split[0]: User information
    // split[1]: Host identifiers;
    auto t2 = partitionBackward(userAndHostInfo, '@');
    StringData userInfo = std::get<0>(t2);
    StringData hostIdentifiers = std::get<1>(t2);

    // 4. Validate, split (if applicable), and URL decode the user information, yielding:
    // split[0] = username
    // split[1] = password
    auto t3 = partitionForward(userInfo, ':');
    StringData usernameSD = std::get<0>(t3);
    StringData passwordSD = std::get<1>(t3);
    std::size_t found = usernameSD.toString().find_first_of(":@");
    if (found != std::string::npos) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Username must be URL Encoded for mongodb:// URL: " << url);
    }
    found = passwordSD.toString().find_first_of(":@");
    if (found != std::string::npos) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Password must be URL Encoded for mongodb:// URL: " << url);
    }

    // Get the username and make sure it did not fail to decode
    auto usernameWithStatus = urlDecode(usernameSD);
    if (!usernameWithStatus.isOK())
        return Status(
            ErrorCodes::FailedToParse,
            str::stream() << "Username cannot properly be URL decoded for mongodb:// URL: " << url);
    std::string username = usernameWithStatus.getValue();

    // Get the password and make sure it did not fail to decode
    auto passwordWithStatus = urlDecode(passwordSD);
    if (!passwordWithStatus.isOK())
        return Status(
            ErrorCodes::FailedToParse,
            str::stream() << "Password cannot properly be URL decoded for mongodb:// URL: " << url);
    std::string password = passwordWithStatus.getValue();

    // 5. Validate, split, and URL decode the host identifiers.
    if (hostIdentifiers.empty()) {
        return Status(ErrorCodes::FailedToParse, "No server(s) specified");
    }
    std::string hostIdentifiersStr = hostIdentifiers.toString();
    std::vector<HostAndPort> servers;
    for (auto i = boost::make_split_iterator(hostIdentifiersStr,
                                             boost::first_finder(",", boost::is_iequal()));
         i != std::remove_reference<decltype((i))>::type{};
         ++i) {
        auto hostWithStatus = urlDecode(boost::copy_range<std::string>(*i));
        if (!hostWithStatus.isOK()) {
            return Status(
                ErrorCodes::FailedToParse,
                str::stream() << "Host cannot properly be URL decoded for mongodb:// URL: " << url);
        }

        auto statusHostAndPort = HostAndPort::parse(hostWithStatus.getValue());
        if (!statusHostAndPort.isOK()) {
            return statusHostAndPort.getStatus();
        }
        servers.push_back(statusHostAndPort.getValue());
    }

    // 6. Split the auth database and connection options string by the first, unescaped ?, yielding:
    // split[0] = auth database
    // split[1] = connection options
    auto t4 = partitionForward(databaseAndOptions, '?');
    StringData databaseSD = std::get<0>(t4);
    StringData connectionOptions = std::get<1>(t4);
    auto databaseWithStatus = urlDecode(databaseSD);
    if (!databaseWithStatus.isOK()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Database name cannot properly be URL decoded for mongodb:// URL: "
                          << url);
    }
    std::string database = databaseWithStatus.getValue();

    // 7. Validate the database contains no prohibited characters
    // Prohibited characters:
    // slash ("/"), backslash ("\"), space (" "), double-quote ("""), or dollar sign ("$")
    // period (".") is also prohibited, but drivers MAY allow periods
    if (!database.empty() &&
        !NamespaceString::validDBName(database,
                                      NamespaceString::DollarInDbNameBehavior::Disallow)) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Database name cannot have reserved characters for mongodb:// URL: "
                          << url);
    }

    // 8. Validate, split, and URL decode the connection options
    std::map<std::string, std::string> options;
    if (!connectionOptions.empty()) {
        std::string connectionOptionsStr = connectionOptions.toString();
        std::size_t foundQ = connectionOptionsStr.find_first_of("?");
        if (foundQ != std::string::npos) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "URI Cannot Contain multiple questions marks for mongodb:// URL: "
                              << url);
        }
        for (auto i = boost::make_split_iterator(connectionOptionsStr,
                                                 boost::first_finder("&", boost::is_iequal()));
             i != std::remove_reference<decltype((i))>::type{};
             ++i) {
            std::string opt = boost::copy_range<std::string>(*i);
            if (opt.empty()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << "Missing a key/value pair in the options for mongodb:// URL: "
                                  << url);
            }
            auto t5 = partitionForward(opt, '=');
            StringData key = std::get<0>(t5);
            StringData value = std::get<1>(t5);
            if (key.empty()) {
                return Status(
                    ErrorCodes::FailedToParse,
                    str::stream()
                        << "Missing a key for key/value pair in the options for mongodb:// URL: "
                        << url);
            }
            if (value.empty()) {
                return Status(
                    ErrorCodes::FailedToParse,
                    str::stream()
                        << "Missing a value for key/value pair in the options for mongodb:// URL: "
                        << url);
            }
            auto keyWithStatus = urlDecode(key);
            if (!keyWithStatus.isOK()) {
                return Status(
                    ErrorCodes::FailedToParse,
                    str::stream()
                        << "Key in options cannot properly be URL decoded for mongodb:// URL: "
                        << url);
            }
            auto valueWithStatus = urlDecode(value);
            if (!valueWithStatus.isOK()) {
                return Status(
                    ErrorCodes::FailedToParse,
                    str::stream()
                        << "Value in options cannot properly be URL decoded for mongodb:// URL: "
                        << url);
            }


            options[keyWithStatus.getValue()] = valueWithStatus.getValue();
        }
    }

    OptionsMap::const_iterator optIter;

    // If a replica set option was specified, store it in the 'setName' field.
    bool haveSetName;
    std::string setName;
    if ((haveSetName = ((optIter = options.find("replicaSet")) != options.end()))) {
        setName = optIter->second;
    }

    const bool direct = !haveSetName && (servers.size() == 1);

    if (!direct && setName.empty()) {
        return Status(ErrorCodes::FailedToParse,
                      "Cannot list multiple servers in URL without 'replicaSet' option");
    }
    ConnectionString cs(
        direct ? ConnectionString::MASTER : ConnectionString::SET, servers, setName);
    return MongoURI(std::move(cs), username, password, database, std::move(options));
}

}  // namespace mongo

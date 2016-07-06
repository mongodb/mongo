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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/password_digest.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/regex.hpp>

namespace mongo {

namespace {
const char kMongoDBURL[] =
    // scheme: non-capturing
    "mongodb://"

    // credentials: two inner captures for user and password
    "(?:([^:]+)(?::([^@]+))?@)?"

    // servers: grabs all host:port or UNIX socket names
    "((?:[^\\/]+|/.+\\.sock)(?:(?:[^\\/]+|/.+\\.sock),)*)"

    // database and options are grouped together
    "(?:/"

    // database: matches anything but the chars that cannot be part of a MongoDB database name which
    // are (in order) - forward slash, back slash, dot, space, double-quote, dollar sign, asterisk,
    // less than, greater than, colon, pipe, question mark.
    "([^/\\\\\\.\\ \"\\$*<>:\\|\\?]*)?"

    // options
    "(?:\\?([^&=?]+=[^&=?]+(?:&[^&=?]+=[^&=?]+)*))?"

    // close db/options group
    ")?";

}  // namespace


StatusWith<MongoURI> MongoURI::parse(const std::string& url) {
    if (!boost::algorithm::starts_with(url, "mongodb://")) {
        auto cs_status = ConnectionString::parse(url);
        if (!cs_status.isOK()) {
            return cs_status.getStatus();
        }

        return MongoURI(cs_status.getValue());
    }

    const boost::regex mongoUrlRe(kMongoDBURL);

    boost::smatch matches;
    if (!boost::regex_match(url, matches, mongoUrlRe)) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Failed to parse mongodb:// URL: " << url);
    }

    // We have the whole input plus 5 top level captures (user, password, host, db, options).
    invariant(matches.size() == 6);

    if (!matches[3].matched) {
        return Status(ErrorCodes::FailedToParse, "No server(s) specified");
    }

    std::map<std::string, std::string> options;

    if (matches[5].matched) {
        const std::string optionsMatch = matches[5].str();

        std::vector<boost::iterator_range<std::string::const_iterator>> optionsTokens;
        boost::algorithm::split(optionsTokens, optionsMatch, boost::algorithm::is_any_of("=&"));

        if (optionsTokens.size() % 2 != 0) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Missing a key or value in the options for mongodb:// URL: "
                              << url);
            ;
        }

        for (size_t i = 0; i != optionsTokens.size(); i = i + 2)
            options[std::string(optionsTokens[i].begin(), optionsTokens[i].end())] =
                std::string(optionsTokens[i + 1].begin(), optionsTokens[i + 1].end());
    }

    OptionsMap::const_iterator optIter;

    // If a replica set option was specified, store it in the 'setName' field.
    bool haveSetName;
    std::string setName;
    if ((haveSetName = ((optIter = options.find("replicaSet")) != options.end()))) {
        setName = optIter->second;
    }

    std::vector<HostAndPort> servers;

    {
        std::vector<std::string> servers_split;
        const std::string serversStr = matches[3].str();
        boost::algorithm::split(servers_split, serversStr, boost::is_any_of(","));
        for (auto&& s : servers_split) {
            auto statusHostAndPort = HostAndPort::parse(s);
            if (!statusHostAndPort.isOK()) {
                return statusHostAndPort.getStatus();
            }

            servers.push_back(statusHostAndPort.getValue());
        }
    }

    const bool direct = !haveSetName && (servers.size() == 1);

    if (!direct && setName.empty()) {
        return Status(ErrorCodes::FailedToParse,
                      "Cannot list multiple servers in URL without 'replicaSet' option");
    }

    ConnectionString cs(
        direct ? ConnectionString::MASTER : ConnectionString::SET, servers, setName);
    return MongoURI(
        std::move(cs), matches[1].str(), matches[2].str(), matches[4].str(), std::move(options));
}

}  // namespace mongo

/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "bongo/base/status_with.h"
#include "bongo/base/string_data.h"
#include "bongo/bson/bsonobj.h"
#include "bongo/client/connection_string.h"
#include "bongo/stdx/mutex.h"
#include "bongo/util/assert_util.h"
#include "bongo/util/net/hostandport.h"

namespace bongo {

/**
 * BongoURI handles parsing of URIs for bongodb, and falls back to old-style
 * ConnectionString parsing. It's used primarily by the shell.
 * It parses URIs with the following format:
 *
 *    bongodb://[usr:pwd@]host1[:port1]...[,hostN[:portN]]][/[db][?options]]
 *
 * For a complete list of URI string options, see
 * https://wiki.bongodb.com/display/DH/Connection+String+Format
 *
 * Examples:
 *
 *    A replica set with three members (one running on default port 27017):
 *      string uri = bongodb://localhost,localhost:27018,localhost:27019
 *
 *    Authenticated connection to db 'bedrock' with user 'barney' and pwd 'rubble':
 *      string url = bongodb://barney:rubble@localhost/bedrock
 *
 *    Use parse() to parse the url, then validate and connect:
 *      string errmsg;
 *      ConnectionString cs = ConnectionString::parse( url, errmsg );
 *      if ( ! cs.isValid() ) throw "bad connection string: " + errmsg;
 *      DBClientBase * conn = cs.connect( errmsg );
 */
class BongoURI {
public:
    using OptionsMap = std::map<std::string, std::string>;

    static StatusWith<BongoURI> parse(const std::string& url);

    DBClientBase* connect(StringData applicationName,
                          std::string& errmsg,
                          boost::optional<double> socketTimeoutSecs = boost::none) const;

    const std::string& getUser() const {
        return _user;
    }

    const std::string& getPassword() const {
        return _password;
    }

    const OptionsMap& getOptions() const {
        return _options;
    }

    const std::string& getDatabase() const {
        return _database;
    }

    bool isValid() const {
        return _connectString.isValid();
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

    // If you are trying to clone a URI (including its options/auth information) for a single
    // server (say a member of a replica-set), you can pass in its HostAndPort information to
    // get a new URI with the same info, except type() will be MASTER and getServers() will
    // be the single host you pass in.
    BongoURI cloneURIForServer(const HostAndPort& hostAndPort) const {
        return BongoURI(ConnectionString(hostAndPort), _user, _password, _database, _options);
    }

    ConnectionString::ConnectionType type() const {
        return _connectString.type();
    }

    explicit BongoURI(const ConnectionString connectString)
        : _connectString(std::move(connectString)){};

    BongoURI() = default;

private:
    BongoURI(ConnectionString connectString,
             const std::string& user,
             const std::string& password,
             const std::string& database,
             OptionsMap options)
        : _connectString(std::move(connectString)),
          _user(user),
          _password(password),
          _database(database),
          _options(std::move(options)){};

    BSONObj _makeAuthObjFromOptions(int maxWireVersion) const;

    ConnectionString _connectString;
    std::string _user;
    std::string _password;
    std::string _database;
    OptionsMap _options;
};

}  // namespace bongo

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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * MongoURI handles parsing of URIs for mongodb, and falls back to old-style
 * ConnectionString parsing. It's used primarily by the shell.
 * It parses URIs with the following format:
 *
 *    mongodb://[usr:pwd@]host1[:port1]...[,hostN[:portN]]][/[db][?options]]
 *
 * For a complete list of URI string options, see
 * https://wiki.mongodb.com/display/DH/Connection+String+Format
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
    using OptionsMap = std::map<std::string, std::string>;

    static StatusWith<MongoURI> parse(const std::string& url);

    DBClientBase* connect(std::string& errmsg) const;

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

    ConnectionString::ConnectionType type() const {
        return _connectString.type();
    }

    explicit MongoURI(const ConnectionString connectString)
        : _connectString(std::move(connectString)){};

    MongoURI() = default;

private:
    MongoURI(ConnectionString connectString,
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

}  // namespace mongo

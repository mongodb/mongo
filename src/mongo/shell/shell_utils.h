// mongo/shell/shell_utils.h
/*
 *    Copyright 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

class Scope;
class DBClientWithCommands;

namespace shell_utils {

extern std::string _dbConnect;
extern std::string _dbAuth;
extern bool _nokillop;

void RecordMyLocation(const char* _argv0);
void installShellUtils(Scope& scope);

void initScope(Scope& scope);
void onConnect(DBClientWithCommands& c);

const char* getUserDir();

BSONElement singleArg(const BSONObj& args);
extern const BSONObj undefinedReturn;

/** Prompt for confirmation from cin. */
class Prompter {
public:
    Prompter(const std::string& prompt);
    /** @return prompted confirmation or cached confirmation. */
    bool confirm();

private:
    const std::string _prompt;
    bool _confirmed;
};

/** Registry of server connections. */
class ConnectionRegistry {
public:
    ConnectionRegistry();
    void registerConnection(DBClientWithCommands& client);
    void killOperationsOnAllConnections(bool withPrompt) const;

private:
    std::map<std::string, std::set<std::string>> _connectionUris;
    mutable stdx::mutex _mutex;
};

extern ConnectionRegistry connectionRegistry;

// This mutex helps the shell serialize output on exit, to avoid deadlocks at shutdown. So
// it also protects the global dbexitCalled.
extern stdx::mutex& mongoProgramOutputMutex;

// Helper to tell if a file exists cross platform
// TODO: Remove this when we have a cross platform file utility library
bool fileExists(const std::string& file);
}
}

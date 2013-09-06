// config_server_tests.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/s/config.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    // Note: these are all crutch and hopefully will eventually go away
    CmdLine cmdLine;

    bool inShutdown() {
        return false;
    }

    DBClientBase *createDirectClient() { return NULL; }

    void dbexit(ExitCode rc, const char *why){
        ::_exit(-1);
    }

    bool haveLocalShardingInfo(const string& ns) {
        return false;
    }

namespace {

    using std::string;
    using std::vector;

    ConfigServer configServer;
    const string serverUrl1 = "server1:27001";
    const string serverUrl2 = "server2:27002";
    const string serverUrl3 = "server3:27003";

    TEST(sharding, ConfigHostCheck3Different) {

        //Test where all three are different - Expected to return true
        vector<string> configHosts;
        string errmsg1;
        configHosts.push_back(serverUrl1);
        configHosts.push_back(serverUrl2);
        configHosts.push_back(serverUrl3);
        ASSERT_TRUE(configServer.checkHostsAreUnique(configHosts, &errmsg1));
        ASSERT_TRUE(errmsg1.empty());
    }

    TEST(sharding, ConfigHostCheckOneHost) {

        //Test short circuit with one Host - Expected to return true
        vector<string> configHosts;
        string errmsg2;
        configHosts.push_back(serverUrl2);
        ASSERT_TRUE(configServer.checkHostsAreUnique(configHosts, &errmsg2));
        ASSERT_TRUE(errmsg2.empty());
    }

    TEST(sharding, ConfigHostCheckTwoIdentical) {
        //Test with two identical hosts - Expected to return false
        vector<string> configHosts;
        string errmsg3;
        configHosts.push_back(serverUrl1);
        configHosts.push_back(serverUrl2);
        configHosts.push_back(serverUrl2);
        ASSERT_FALSE(configServer.checkHostsAreUnique(configHosts, &errmsg3));
        ASSERT_FALSE(errmsg3.empty());
    }

} // unnamed namespace

} // namespace mongo

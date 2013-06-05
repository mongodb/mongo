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

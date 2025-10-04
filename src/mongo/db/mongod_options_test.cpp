/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/mongod_options.h"

#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/value.h"

namespace mongo {
namespace {

class MongodOptionsTest : public unittest::Test {
public:
    class Environment : public moe::Environment {
    public:
        auto& setPort(int port) {
            _set("net.port", port);
            return *this;
        }

        auto& setClusterRole(std::string role) {
            _set("sharding.clusterRole", role);
            return *this;
        }

        auto& setReplicaSet(std::string rs) {
            _set("replication.replSet", rs);
            return *this;
        }

        auto& setMagicRestore() {
            _set("magicRestore", true);
            return *this;
        }

    private:
        template <typename K, typename V>
        void _set(K key, V value) {
            uassertStatusOK(set(moe::Key(key), moe::Value(value)));
        }
    };

    void setUp() override {
        // Note that the tests are currently only focused on the cluster role and listening ports,
        // so we only reset the relevant states between runs.
        ServerGlobalParams defaults;
        serverGlobalParams.port = defaults.port;
        serverGlobalParams.clusterRole = defaults.clusterRole;

        env = Environment{};
    }

    Environment env;
};

TEST_F(MongodOptionsTest, Base) {
    ASSERT_OK(storeMongodOptions(env));
}

TEST_F(MongodOptionsTest, MagicRestoreNoReplicaSet) {
    env.setMagicRestore();
    auto status = storeMongodOptions(env);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(), "Cannot start magic restore without --replSet");

    env.setReplicaSet("rsName").setMagicRestore();
    ASSERT_OK(storeMongodOptions(env));
}

TEST_F(MongodOptionsTest, MagicRestoreDefaultPort) {
    env.setReplicaSet("rsName").setMagicRestore();
    ASSERT_OK(storeMongodOptions(env));

    ASSERT_EQ(serverGlobalParams.port, ServerGlobalParams::DefaultMagicRestorePort);
}

TEST_F(MongodOptionsTest, MagicRestoreUseProvidedPort) {
    env.setReplicaSet("rsName").setMagicRestore().setPort(123);
    ASSERT_OK(storeMongodOptions(env));

    ASSERT_EQ(serverGlobalParams.port, 123);
}

TEST_F(MongodOptionsTest, MagicRestoreShardParams) {
    env.setMagicRestore().setReplicaSet("rsName").setClusterRole("shardsvr");
    auto status = storeMongodOptions(env);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "Cannot start magic restore with --shardsvr or --configsvr");

    env.setMagicRestore().setReplicaSet("rsName").setClusterRole("configsvr");
    status = storeMongodOptions(env);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "Cannot start magic restore with --shardsvr or --configsvr");
}

}  // namespace
}  // namespace mongo

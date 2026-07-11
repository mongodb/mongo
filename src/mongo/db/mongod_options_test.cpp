// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/mongod_options.h"

#include "mongo/db/global_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
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

        auto& setOplogMinRetentionHours(double hours) {
            _set("storage.oplogMinRetentionHours", hours);
            return *this;
        }

        auto& setOplogSizeMB(int sizeMB) {
            _set("replication.oplogSizeMB", sizeMB);
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

TEST_F(MongodOptionsTest, OplogMinRetentionUnset) {
    ASSERT_OK(storeMongodOptions(env));
    ASSERT_EQ(storageGlobalParams.oplogMinRetentionHours.load(), 0.0);
    ASSERT_TRUE(storageGlobalParams.oplogMinRetentionInitializedUsingDefault);
}

TEST_F(MongodOptionsTest, OplogMinRetentionZeroValid) {
    env.setOplogMinRetentionHours(0);
    ASSERT_OK(storeMongodOptions(env));
    ASSERT_EQ(storageGlobalParams.oplogMinRetentionHours.load(), 0.0);
    ASSERT_FALSE(storageGlobalParams.oplogMinRetentionInitializedUsingDefault);
}

TEST_F(MongodOptionsTest, OplogMinRetentionValueValid) {
    env.setOplogMinRetentionHours(5.5);
    ASSERT_OK(storeMongodOptions(env));
    ASSERT_EQ(storageGlobalParams.oplogMinRetentionHours.load(), 5.5);
    ASSERT_FALSE(storageGlobalParams.oplogMinRetentionInitializedUsingDefault);
}

TEST_F(MongodOptionsTest, OplogMinRetentionNegativeInvalid) {
    env.setOplogMinRetentionHours(-100);
    auto status = storeMongodOptions(env);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "bad --oplogMinRetentionHours, argument must be greater or equal to 0");
}

TEST_F(MongodOptionsTest, OplogSizeValid) {
    env.setOplogSizeMB(1000);
    ASSERT_OK(storeMongodOptions(env));
    ASSERT_EQ(getGlobalReplSettings().getOplogSizeBytes(), 1000LL * 1024 * 1024);
    ASSERT_FALSE(getGlobalReplSettings().isOplogSizeInitializedUsingDefault());
}

TEST_F(MongodOptionsTest, OplogSizeZeroInvalid) {
    env.setOplogSizeMB(0);
    auto status = storeMongodOptions(env);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(), "bad --oplogSize");
}

TEST_F(MongodOptionsTest, OplogSizeNegativeInvalid) {
    env.setOplogSizeMB(-1);
    auto status = storeMongodOptions(env);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(), "bad --oplogSize");
}

}  // namespace
}  // namespace mongo

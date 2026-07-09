/**
 * Tests how a config-only mongoS routes reads depending on the target namespace, against a standby
 * config server. The cases below cover the distinct routing equivalence classes:
 *
 *   1. Always-untracked 'admin.*' reads (e.g. admin.system.version) -- resolved to a fixed entry on
 *      the config server and served by forwarding the read there.
 *   2. Always-untracked 'config.*' reads (e.g. config.settings) -- same as above.
 *   3. The tracked exception 'config.system.sessions' -- not always-untracked, so it goes through
 *      the collection-cache refresh (reads the catalog collections), which is correctly rejected
 *      in config-only mode (code 12319005).
 *   4. A non-fixed user database read -- rejected because the database cannot be resolved without
 *      reading the catalog (code 12319007).
 *
 * @tags: [
 *   requires_sharding,
 *   requires_persistence,
 * ]
 */

import {StandbyClusterTestFixture} from "jstests/noPassthrough/libs/sharded_cluster_topology/standby_cluster_test_fixture.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// Config-only mode error codes (see sharding_catalog_client_impl.cpp).
const kCannotAccessCollection = 12319005; // getCollectionAndChunks: catalog collection read blocked.
const kCannotAccessDatabase = 12319007; // getDatabase: non-fixed database read blocked.

// Reads target a serving secondary; the INJECTOR-tagged primary is excluded from server selection
// in a standby cluster.
const kSecondaryPreferred = {mode: "secondaryPreferred"};

for (const configShard of [false, true]) {
    describe(`config-only mode fixed-namespace reads [configShard=${configShard}]`, function () {
        before(function () {
            const fixtureOpts = {name: jsTestName(), shards: 1, configShard};
            if (configShard) {
                fixtureOpts.rs = {nodes: 2};
            } else {
                fixtureOpts.rs = {nodes: 1};
                fixtureOpts.config = 2;
            }
            this.fixture = new StandbyClusterTestFixture(fixtureOpts);

            this.fixture.transitionToStandby();

            this.mongos = MongoRunner.runMongos({
                configdb: this.fixture.standbyRS.getURL(),
                configOnly: "",
                setParameter: {defaultConfigCommandTimeoutMS: 5000},
            });
            assert.neq(null, this.mongos, "mongoS failed to start against standby config server");
            assert.commandWorked(this.mongos.adminCommand({hello: 1}));
        });

        after(function () {
            if (this.mongos) {
                MongoRunner.stopMongos(this.mongos);
            }
            this.fixture.teardown();
        });

        it("serves reads on always-untracked admin.* namespaces", function () {
            assert.commandWorked(
                this.mongos.getDB("admin").runCommand({
                    find: "system.version",
                    filter: {},
                    $readPreference: kSecondaryPreferred,
                    maxTimeMS: 5000,
                }),
                "read on admin.system.version should be served by the config server",
            );
        });

        it("serves reads on always-untracked config.* namespaces", function () {
            assert.commandWorked(
                this.mongos.getDB("config").runCommand({
                    find: "settings",
                    filter: {},
                    $readPreference: kSecondaryPreferred,
                    maxTimeMS: 5000,
                }),
                "read on config.settings should be served by the config server",
            );
        });

        it("rejects reads on the tracked config.system.sessions namespace", function () {
            // config.system.sessions is the one config namespace that is not always-untracked, so
            // its routing requires reading the catalog collections, which is blocked.
            assert.commandFailedWithCode(
                this.mongos.getDB("config").runCommand({
                    find: "system.sessions",
                    filter: {},
                    $readPreference: kSecondaryPreferred,
                    maxTimeMS: 5000,
                }),
                kCannotAccessCollection,
            );
        });

        it("rejects reads on non-fixed user databases", function () {
            assert.commandFailedWithCode(
                this.mongos.getDB("someUserDb").runCommand({
                    find: "someColl",
                    filter: {},
                    $readPreference: kSecondaryPreferred,
                    maxTimeMS: 5000,
                }),
                kCannotAccessDatabase,
            );
        });
    });
}

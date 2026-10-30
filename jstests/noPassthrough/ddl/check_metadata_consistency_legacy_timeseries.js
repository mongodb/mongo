/**
 * Tests that checkMetadataConsistency reports legacy time-series collections at cluster, database
 * and collection scope.
 */

import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("CMC legacy time-series collections", function () {
    before(function () {
        this.st = new ShardingTest({shards: 1, rs: {nodes: 1}});
        this.dbName = jsTestName();
        this.db = this.st.s.getDB(this.dbName);
        this.checkOptions = {_checkSecondariesMode: "noSecondaryCheck"};
        this.legacyName = "legacy";
        this.bucketsName = "system.buckets." + this.legacyName;
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );

        this.checkCluster = () =>
            this.st.s.getDB("admin").checkMetadataConsistency(this.checkOptions).toArray();
        this.checkDb = () => this.db.checkMetadataConsistency(this.checkOptions).toArray();
        this.checkColl = (collName) =>
            this.db.getCollection(collName).checkMetadataConsistency(this.checkOptions).toArray();

        this.assertOneLegacyInconsistency = (inconsistencies) => {
            assert.eq(1, inconsistencies.length, {inconsistencies});
            const inc = inconsistencies[0];
            assert.eq("LegacyTimeseriesCollectionsPresent", inc.type, {inc});
            assert.eq(`${this.dbName}.${this.bucketsName}`, inc.details.namespace, {inc});
            assert.eq(this.st.shard0.shardName, inc.details.shard, {inc});
        };
    });

    after(function () {
        assert.commandWorked(this.db.dropDatabase());
        this.st.stop();
    });

    it("reports no inconsistency for a healthy viewless collection", function () {
        assert.commandWorked(this.db.createCollection("healthy", {timeseries: {timeField: "t"}}));

        let inconsistencies = this.checkCluster();
        assert.eq(0, inconsistencies.length, {inconsistencies});

        inconsistencies = this.checkDb();
        assert.eq(0, inconsistencies.length, {inconsistencies});

        inconsistencies = this.checkColl("healthy");
        assert.eq(0, inconsistencies.length, {inconsistencies});
    });

    it("reports the legacy collection at cluster, database and collection scope", function () {
        const primary = this.st.rs0.getPrimary();
        const shardDB = primary.getDB(this.dbName);

        // Use a failpoint to bypass the format guard and create a legacy timeseries collection
        // in FCV 9.0+.
        withFailPoint(primary, "skipCreateTimeseriesVersionMismatchCheck", () => {
            assert.commandWorked(
                shardDB.runCommand({
                    applyOps: [
                        {
                            op: "c",
                            ns: `${this.dbName}.$cmd`,
                            o: {create: "system.views"},
                        },
                        {
                            op: "c",
                            ns: `${this.dbName}.$cmd`,
                            o: {
                                create: this.bucketsName,
                                clusteredIndex: true,
                                timeseries: {
                                    timeField: "t",
                                    granularity: "seconds",
                                    bucketMaxSpanSeconds: 3600,
                                },
                            },
                        },
                        {
                            op: "i",
                            ns: `${this.dbName}.system.views`,
                            o: {
                                _id: `${this.dbName}.${this.legacyName}`,
                                viewOn: this.bucketsName,
                                pipeline: [
                                    {
                                        $_internalUnpackBucket: {
                                            timeField: "t",
                                            bucketMaxSpanSeconds: 3600,
                                        },
                                    },
                                ],
                            },
                        },
                    ],
                }),
            );
        });

        this.assertOneLegacyInconsistency(this.checkCluster());
        this.assertOneLegacyInconsistency(this.checkDb());
        this.assertOneLegacyInconsistency(this.checkColl(this.legacyName));
    });
});

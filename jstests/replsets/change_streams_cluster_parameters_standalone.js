/**
 * Checks if the changeStreams and changeStreamOptions parameters allow standalone instances to
 * initiate.
 *
 * @tags: [
 *  does_not_support_stepdowns,
 *  multiversion_incompatible,
 *  requires_replication,
 *  requires_persistence,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

(function () {
    "use strict";

    const kExpireAfterSeconds = NumberLong(7177);

    jsTestLog("changeStreamOptions cluster parameter must not prevent a standalone node to start");
    {
        const replicaSet = new ReplSetTest({name: "replSet2", nodes: 3});
        replicaSet.startSet();
        replicaSet.initiate();
        const primary = replicaSet.getPrimary();
        assert.commandWorked(
            primary.getDB("admin").runCommand({
                setClusterParameter: {
                    changeStreamOptions: {preAndPostImages: {expireAfterSeconds: kExpireAfterSeconds}},
                },
            }),
        );

        replicaSet.awaitReplication();

        replicaSet.stopSet(/*signal=*/ null, /*forRestart=*/ true);

        const primaryStandalone = MongoRunner.runMongod({dbpath: primary.dbpath, noReplSet: true, noCleanData: true});

        assert.eq(primaryStandalone.getDB("config").clusterParameters.countDocuments({_id: "changeStreamOptions"}), 1);

        MongoRunner.stopMongod(primaryStandalone);

        replicaSet.startSet({restart: true});
        replicaSet.stopSet();
    }
})();

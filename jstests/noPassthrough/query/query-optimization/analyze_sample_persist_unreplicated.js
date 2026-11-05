/**
 * Tests that the analyze command with mode: "sample" fails on unreplicated collections and does not
 * persist a sample.
 *
 * @tags: [
 *   # Requires featureFlagPersistentStats for persisted samples.
 *   featureFlagPersistentStats,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

describe("analyze with mode: 'sample' on unreplicated collections", function () {
    let conn;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: true,
            },
        });
        assert.neq(null, conn);
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("fails on a collection in the local database", function () {
        const localDb = conn.getDB("local");
        const localColl = localDb[jsTestName()];
        localColl.drop();
        assert.commandWorked(localColl.insertMany([{a: 1}, {a: 2}, {a: 3}]));
        assert.commandFailedWithCode(
            localDb.runCommand({
                analyze: localColl.getName(),
                mode: "sample",
                samplingMethod: "random",
            }),
            ErrorCodes.IllegalOperation,
            "analyze on a collection in the local database should fail with IllegalOperation",
        );
        assert.eq(PersistentSamplesUtils.getSamplesColl(localDb).countDocuments({}), 0);
        localColl.drop();
    });

    it("fails on system.profile", function () {
        const db = conn.getDB("test");
        assert.commandWorked(db.setProfilingLevel(2));
        assert.commandWorked(db.runCommand({ping: 1}));
        assert.commandWorked(db.setProfilingLevel(0));
        assert.commandFailedWithCode(
            db.runCommand({analyze: "system.profile", mode: "sample", samplingMethod: "random"}),
            ErrorCodes.IllegalOperation,
            "analyze on system.profile should fail with IllegalOperation",
        );
        assert.eq(PersistentSamplesUtils.getSamplesColl(db).countDocuments({}), 0);
        db.system.profile.drop();
    });
});

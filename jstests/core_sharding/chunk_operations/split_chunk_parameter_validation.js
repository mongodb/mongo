/**
 * Tests the split command's parameter validation on mongos. Covers mutually exclusive parameter
 * combinations, missing required parameters, malformed bounds arrays, and partial shard keys
 * on compound-key collections. These validation paths live in cluster_split_cmd.cpp's
 * errmsgRun() and are not exercised by the other split tests (basic_split.js, etc.).
 *
 * @tags: [
 *  assumes_balancer_off,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("split command parameter validation", function () {
    const dbName = db.getName();
    const admin = db.getSiblingDB("admin");

    const simpleCollName = jsTestName() + "_simple";
    const simpleNs = dbName + "." + simpleCollName;

    const compoundCollName = jsTestName() + "_compound";
    const compoundNs = dbName + "." + compoundCollName;

    before(function () {
        assert.commandWorked(admin.runCommand({shardCollection: simpleNs, key: {_id: 1}}));
        assert.commandWorked(admin.runCommand({shardCollection: compoundNs, key: {x: 1, y: 1}}));
    });

    after(function () {
        db.getCollection(simpleCollName).drop();
        db.getCollection(compoundCollName).drop();
    });

    describe("mutually exclusive parameters", function () {
        it("rejects find and bounds together", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: simpleNs,
                    find: {_id: 0},
                    bounds: [{_id: MinKey}, {_id: MaxKey}],
                }),
            );
        });

        it("rejects find and middle together", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: simpleNs,
                    find: {_id: 0},
                    middle: {_id: 0},
                }),
            );
        });

        it("rejects bounds and middle together", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: simpleNs,
                    bounds: [{_id: MinKey}, {_id: MaxKey}],
                    middle: {_id: 0},
                }),
            );
        });

        it("rejects all three parameters together", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: simpleNs,
                    find: {_id: 0},
                    bounds: [{_id: MinKey}, {_id: MaxKey}],
                    middle: {_id: 0},
                }),
            );
        });
    });

    describe("missing parameters", function () {
        it("rejects split with no find, bounds, or middle", function () {
            assert.commandFailed(admin.runCommand({split: simpleNs}));
        });
    });

    describe("incomplete bounds", function () {
        it("rejects bounds with only lower bound", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: simpleNs,
                    bounds: [{_id: MinKey}],
                }),
            );
        });

        it("rejects bounds with empty array", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: simpleNs,
                    bounds: [],
                }),
            );
        });
    });

    describe("partial shard key on compound key collection", function () {
        it("rejects middle with partial compound shard key", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: compoundNs,
                    middle: {x: 0},
                }),
            );
        });

        it("rejects find with partial compound shard key", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: compoundNs,
                    find: {x: 0},
                }),
            );
        });

        it("rejects bounds with partial compound shard key", function () {
            assert.commandFailed(
                admin.runCommand({
                    split: compoundNs,
                    bounds: [{x: MinKey}, {x: MaxKey}],
                }),
            );
        });
    });
});

/**
 * Test the basic operation of a `$_internalSearchIdLookup` aggregation stage.
 *
 * `$_internalSearchIdLookup` resolves `_id`s through one of two executors depending on the
 * `featureFlagSearchOptimizedIdLookup` IFR flag: the Express fast path (flag on) or the classic
 * local-read path (flag off). Because the flag is toggleable at runtime, we run the suite once per
 * state so the waterfall exercises both executors.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = "internal_search_id_lookup";

const expectedEvenIdDocs = [
    {_id: 2, x: "now", y: "lorem"},
    {_id: 4, x: "cow", y: "lorem ipsum"},
    {_id: 6, x: "cow", y: "lorem ipsum"},
    {_id: 8, x: "cow", y: "lorem ipsum"},
];

// TODO SERVER-130493 Remove optimizedIdLookup when no-ff search variant exists.
for (const optimizedIdLookup of [false, true]) {
    describe(
        `$_internalSearchIdLookup basic operation ` +
            `(featureFlagSearchOptimizedIdLookup=${optimizedIdLookup})`,
        function () {
            before(function () {
                this.conn = MongoRunner.runMongod();
                this.db = this.conn.getDB("test");
                assert.commandWorked(
                    this.db.adminCommand({
                        setParameter: 1,
                        featureFlagSearchOptimizedIdLookup: optimizedIdLookup,
                    }),
                );

                const coll = this.db[collName];
                coll.drop();
                assert.writeOK(coll.insert({_id: 1, x: "ow"}));
                assert.writeOK(coll.insert({_id: 2, x: "now", y: "lorem"}));
                assert.writeOK(coll.insert({_id: 3, x: "brown", y: "ipsum"}));
                assert.writeOK(coll.insert({_id: 4, x: "cow", y: "lorem ipsum"}));
                assert.writeOK(coll.insert({_id: 5, x: "brown", y: "ipsum"}));
                assert.writeOK(coll.insert({_id: 6, x: "cow", y: "lorem ipsum"}));
                assert.writeOK(coll.insert({_id: 7, x: "brown", y: "ipsum"}));
                assert.writeOK(coll.insert({_id: 8, x: "cow", y: "lorem ipsum"}));
            });

            after(function () {
                MongoRunner.stopMongod(this.conn);
            });

            it("skips `_id`s it cannot find", function () {
                assert.eq(
                    4,
                    this.db[collName]
                        .aggregate([
                            {$match: {}},
                            {$addFields: {idToLookFor: {$toInt: "$_id"}}},
                            {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
                            {$_internalSearchIdLookup: {}},
                        ])
                        .itcount(),
                );
            });

            it("returns the expected documents after skipping some `_id`s", function () {
                assert.eq(
                    expectedEvenIdDocs,
                    this.db[collName]
                        .aggregate([
                            {$match: {}},
                            {$addFields: {idToLookFor: {$toInt: "$_id"}}},
                            {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
                            {$_internalSearchIdLookup: {}},
                            {$sort: {"_id": 1}},
                        ])
                        .toArray(),
                );
            });

            it("returns nothing when the collection does not exist", function () {
                assert.eq(
                    [],
                    this.db["nonexistentColl"]
                        .aggregate([
                            {$match: {}},
                            {$addFields: {idToLookFor: {$toInt: "$_id"}}},
                            {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
                            {$_internalSearchIdLookup: {}},
                        ])
                        .toArray(),
                );
            });

            it("uasserts when `_id` is populated but no collection is specified", function () {
                // $_internalSearch should uassert when a collection is unspecified and the source
                // stage provides documents with `_id` populated.
                assert.commandFailedWithCode(
                    this.db.runCommand({
                        aggregate: 1,
                        pipeline: [
                            {$listLocalSessions: {allUsers: true}},
                            {$addFields: {"_id": ObjectId("5ab9cbfa31c2ab715d42129e")}},
                            {$_internalSearchIdLookup: {}},
                        ],
                        cursor: {},
                    }),
                    11140100,
                );
            });
        },
    );
}

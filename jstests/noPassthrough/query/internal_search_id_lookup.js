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

// Runs an aggregate on the internal client connection. $_internalSearchIdLookup is an internal-only
// stage: it is rejected in user requests and only accepted from an internal client. internalClient
// connections must specify an explicit writeConcern on every command that accepts one (getMores are
// exempt, and DBCommandCursor omits it for us), so include an empty writeConcern.
function idLookupAgg(internalDB, collectionName, pipeline) {
    const res = assert.commandWorked(
        internalDB.runCommand({
            aggregate: collectionName,
            pipeline: pipeline,
            cursor: {},
            readConcern: {},
            writeConcern: {},
        }),
    );
    return new DBCommandCursor(internalDB, res);
}

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

                // Create an internal client connection to exercise the internal-only stage, leaving
                // the main connection as a normal client so that setup writes and shutdown-time
                // validation hooks are unaffected.
                this.internalConn = new Mongo(this.db.getMongo().host);
                assert.commandWorked(
                    this.internalConn.getDB("admin").runCommand({
                        hello: 1,
                        internalClient: {
                            minWireVersion: NumberInt(0),
                            maxWireVersion: NumberInt(7),
                        },
                    }),
                );
                this.internalDB = this.internalConn.getDB("test");
            });

            after(function () {
                this.internalConn.close();
                MongoRunner.stopMongod(this.conn);
            });

            it("skips `_id`s it cannot find", function () {
                assert.eq(
                    4,
                    idLookupAgg(this.internalDB, collName, [
                        {$match: {}},
                        {$addFields: {idToLookFor: {$toInt: "$_id"}}},
                        {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
                        {$_internalSearchIdLookup: {}},
                    ]).itcount(),
                );
            });

            it("returns the expected documents after skipping some `_id`s", function () {
                assert.eq(
                    expectedEvenIdDocs,
                    idLookupAgg(this.internalDB, collName, [
                        {$match: {}},
                        {$addFields: {idToLookFor: {$toInt: "$_id"}}},
                        {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
                        {$_internalSearchIdLookup: {}},
                        {$sort: {"_id": 1}},
                    ]).toArray(),
                );
            });

            it("returns nothing when the collection does not exist", function () {
                assert.eq(
                    [],
                    idLookupAgg(this.internalDB, "nonexistentColl", [
                        {$match: {}},
                        {$addFields: {idToLookFor: {$toInt: "$_id"}}},
                        {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
                        {$_internalSearchIdLookup: {}},
                    ]).toArray(),
                );
            });

            it("uasserts when `_id` is populated but no collection is specified", function () {
                // $_internalSearch should uassert when a collection is unspecified and the source
                // stage provides documents with `_id` populated.
                assert.commandFailedWithCode(
                    this.internalDB.runCommand({
                        aggregate: 1,
                        pipeline: [
                            {$listLocalSessions: {allUsers: true}},
                            {$addFields: {"_id": ObjectId("5ab9cbfa31c2ab715d42129e")}},
                            {$_internalSearchIdLookup: {}},
                        ],
                        cursor: {},
                        readConcern: {},
                        writeConcern: {},
                    }),
                    11140100,
                );
            });
        },
    );
}

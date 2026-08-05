/**
 * Tests that '--validateParallel' dispatches collections largest-first, so that a large collection
 * is never left as a long tail after the smaller ones have already completed.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

import {parseValidateOutputsFromLogs} from "jstests/noPassthrough/validate/libs/validate_find_repl_set_divergence.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const kNumCollections = 10;
const kCloselySpacedDocCounts = Array.from({length: kNumCollections}, (_, i) => i + 1);

const kTestCases = [
    {
        name: "sizes are distinct but closely spaced",
        docCounts: kCloselySpacedDocCounts,
        dbNames: ["test"],
    },
    {
        // One collection whose validation cost dominates all the others put together. This is the
        // 'long tail' that largest-first ordering exists to avoid.
        name: "one collection is much larger than the rest",
        docCounts: [...kCloselySpacedDocCounts.slice(0, kNumCollections - 1), 500],
        dbNames: ["test"],
    },
    {
        // Collections are prioritized globally rather than database-by-database, so a large
        // collection in one database must be dispatched ahead of a small one in another.
        name: "collections are spread over multiple databases",
        docCounts: kCloselySpacedDocCounts,
        dbNames: ["test", "test2"],
    },
];

for (const testCase of kTestCases) {
    describe(`Parallel modal validate prioritizes large collections when ${testCase.name}`, () => {
        const dbpath = MongoRunner.dataPath + "modal_validate_parallel_ordering";
        const collections = testCase.docCounts.map((docCount, i) => ({
            dbName: testCase.dbNames[i % testCase.dbNames.length],
            collName: `coll${i}`,
            docCount,
        }));
        let expectedOrder;

        before(() => {
            jsTest.log.info("Collections under test", {collections});

            resetDbpath(dbpath);
            const conn = MongoRunner.runMongod({dbpath});

            const padding = "x".repeat(1024);
            for (const {dbName, collName, docCount} of collections) {
                const docs = [];
                for (let i = 0; i < docCount; ++i) {
                    docs.push({_id: i, padding});
                }
                assert.commandWorked(conn.getDB(dbName)[collName].insert(docs));
            }

            const dataSizeForNs = {};
            for (const {dbName, collName} of collections) {
                dataSizeForNs[`${dbName}.${collName}`] = conn.getDB(dbName)[collName].stats().size;
            }

            const dataSizes = Object.values(dataSizeForNs);
            assert.eq(
                new Set(dataSizes).size,
                dataSizes.length,
                "test fixture does not have distinct collection sizes",
                {dataSizeForNs},
            );

            expectedOrder = Object.keys(dataSizeForNs).sort(
                (a, b) => dataSizeForNs[b] - dataSizeForNs[a],
            );

            // A clean shutdown flushes the size storer, so the sizes are available to offline
            // validate.
            MongoRunner.stopMongod(conn);
            clearRawMongoProgramOutput();
        });

        after(() => resetDbpath(dbpath));

        it("validates collections in descending order of data size", () => {
            const dbNameParameter =
                testCase.dbNames.length === 1
                    ? ["--setParameter", `validateDbName=${testCase.dbNames[0]}`]
                    : [];

            assert.eq(
                MongoRunner.EXIT_CLEAN,
                runMongoProgram(
                    "mongod",
                    "--validateParallel",
                    1,
                    "--port",
                    allocatePort(),
                    "--dbpath",
                    dbpath,
                    ...dbNameParameter,
                ),
            );

            const ourNamespaces = new Set(expectedOrder);
            const actualOrder = parseValidateOutputsFromLogs()
                .map((log) => log.attr.results.ns)
                .filter((ns) => ourNamespaces.has(ns));

            assert.eq(expectedOrder, actualOrder, "collections were not validated largest-first");
        });
    });
}

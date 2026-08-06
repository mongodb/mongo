/**
 * Regression tests for BSON that an external data source may supply but that must be rejected at the
 * ingestion boundary, not passed to the query engine where havoc would result:
 *   1. A crafted document whose top-level size is valid but which contains a nested array element
 *      with a negative embedded size (-4 / 0xFFFFFFFC).
 *   2. A well-formed document larger than BSONObjMaxUserSize, a size invariant that the rest of the
 *      server relies on. Covered both on its own and in the sequence that lands it in an
 *      already-grown read buffer, along with the boundary case that must still be accepted.
 *
 * @tags: [
 *   requires_external_data_source,
 * ]
 */
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {
    kMaxLowByteStrLen,
    makeRawStringBson,
    writeBinaryFileInChunks,
} from "jstests/libs/query/raw_bson_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

const kUrlProtocolFile = "file://";

// Fixture file: a single 12-byte BSON document with a valid top-level size but an embedded array
// element "a" whose nested size is -4.
const kMalformedBsonFixture =
    "jstests/noPassthrough/query/external_data_source_malformed_bson_fixture.bson";

// Kept small to pin the boundary exactly; a larger overshoot only makes the fixture bigger.
const kOvershoot = 25;

// A trailing field whose length brings each document's total size to a value that is itself made of
// bytes that survive being written out. See raw_bson_util.js for that constraint.
const kFillerStrLen = (1 << 16) | (1 << 8) | 1;

// Any small length; this document only has to be consumed off the front of a block read.
const kSmallStrLen = 16;

const generatedFixturePaths = [];

describe("BSON ingested from an external data source", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true}});
        this.db = this.conn.getDB(jsTestName());

        // The size invariant under test, as the server itself reports it (BSONObjMaxUserSize).
        const maxBsonObjectSize = assert.commandWorked(this.db.hello()).maxBsonObjectSize;

        // Reading the document at the limit also grows the cursor's buffer to its maximum, which
        // makes its block read size maxBsonObjectSize. The field lengths are chosen so that both
        // totals consist of bytes under the UTF-8 threshold; the assertions below pin that they add
        // up for the limit this server reports, and will fail loudly if that limit ever changes.
        this.maxSizeDoc = makeRawStringBson([
            kMaxLowByteStrLen,
            kMaxLowByteStrLen - kOvershoot,
            kFillerStrLen,
        ]);
        this.oversizedDoc = makeRawStringBson([
            kMaxLowByteStrLen,
            kMaxLowByteStrLen,
            kFillerStrLen,
        ]);
        this.smallDoc = makeRawStringBson([kSmallStrLen]);

        assert.eq(this.maxSizeDoc.length, maxBsonObjectSize, "document must sit at the limit");
        assert.eq(
            this.oversizedDoc.length,
            maxBsonObjectSize + kOvershoot,
            "document must exceed the limit",
        );

        // Writes 'docs' to a generated fixture file and starts an async pipe writer for it,
        // returning the pipe's name. The writer must start before the aggregate is sent, so that
        // mongod opening the pipe for reading unblocks the writer thread (POSIX named pipe
        // semantics).
        this.startPipeWriterForDocs = (docs) => {
            const suffix = extractUUIDFromObject(UUID());
            const fixturePath =
                MongoRunner.dataPath + "external_data_source_bson_" + suffix + ".bson";
            // Generated at runtime rather than committed, as the fixtures run to tens of megabytes.
            writeBinaryFileInChunks(fixturePath, docs.join(""), maxBsonObjectSize / 4);
            generatedFixturePaths.push(fixturePath);

            const pipeName = "external_data_source_bson_" + suffix;
            _writeTestPipeBsonFile(pipeName, docs.length, fixturePath);
            return pipeName;
        };

        // Runs 'pipeline' over 'pipeName' read as an external data source, returning the results.
        this.aggregateOverPipe = (pipeName, pipeline) => {
            return this.db.coll
                .aggregate(pipeline, {
                    $_externalDataSources: [
                        {
                            collName: "coll",
                            dataSources: [
                                {
                                    url: kUrlProtocolFile + pipeName,
                                    storageType: "pipe",
                                    fileType: "bson",
                                },
                            ],
                        },
                    ],
                })
                .toArray();
        };

        // Asserts that reading 'pipeName' fails with 'expectedCode'.
        this.assertAggregateFailsWithCode = (pipeName, expectedCode) => {
            assert.throwsWithCode(
                () => this.aggregateOverPipe(pipeName, [{$project: {a: 1}}]),
                expectedCode,
            );
        };

        // Counts server-side to avoid returning the multi-megabyte payloads to the shell.
        this.assertAggregateReturnsCount = (pipeName, expectedCount) => {
            assert.eq(this.aggregateOverPipe(pipeName, [{$count: "n"}]), [{n: expectedCount}]);
        };
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    afterEach(function () {
        while (generatedFixturePaths.length > 0) {
            removeFile(generatedFixturePaths.pop());
        }
    });

    it("rejects a document with a negative embedded element size", function () {
        const pipeName = "external_data_source_malformed_bson_" + extractUUIDFromObject(UUID());
        _writeTestPipeBsonFile(pipeName, 1, kMalformedBsonFixture);

        this.assertAggregateFailsWithCode(pipeName, 12849400);
    });

    it("accepts a document of exactly BSONObjMaxUserSize", function () {
        this.assertAggregateReturnsCount(this.startPipeWriterForDocs([this.maxSizeDoc]), 1);
    });

    it("rejects a document larger than BSONObjMaxUserSize", function () {
        this.assertAggregateFailsWithCode(
            this.startPipeWriterForDocs([this.oversizedDoc]),
            13251201,
        );
    });

    it("rejects an oversized document that fits in an already-grown buffer", function () {
        // This sequence we want to test:
        //   1. The document at the limit grows the buffer to its maximum, making the block read size
        //      BSONObjMaxUserSize.
        //   2. The small document is consumed off the front of the next block read, leaving the
        //      oversized document near the front of the buffer with most of it buffered.
        //   3. Its remainder fits in the buffer's free tail, so it is read without expanding again.
        const pipeName = this.startPipeWriterForDocs([
            this.maxSizeDoc,
            this.smallDoc,
            this.oversizedDoc,
        ]);

        this.assertAggregateFailsWithCode(pipeName, 13251201);
    });
});

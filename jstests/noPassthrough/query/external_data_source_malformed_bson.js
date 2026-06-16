/**
 * Regression test: a crafted external BSON document whose top-level size is valid but contains a
 * nested array element with a negative embedded size (-4 / 0xFFFFFFFC) must be rejected at the
 * ingestion boundary, not passed to the query engine where havoc would result.
 *
 * @tags: [
 *   requires_external_data_source,
 * ]
 */
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

const kUrlProtocolFile = "file://";

// Fixture file: a single 12-byte BSON document with a valid top-level size but an embedded array
// element "a" whose nested size is -4.
const kMalformedBsonFixture = "jstests/noPassthrough/query/external_data_source_malformed_bson_fixture.bson";

const conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true}});
const db = conn.getDB(jsTestName());

// Start the async pipe writer before sending the aggregate so that when mongod opens the
// named pipe for reading it unblocks the writer thread (POSIX named pipe semantics).
const pipeName = "external_data_source_malformed_bson_" + extractUUIDFromObject(UUID());
_writeTestPipeBsonFile(pipeName, 1, kMalformedBsonFixture);

// The following query must fail because of the malformed BSON.
assert.throwsWithCode(() => {
    db.coll.aggregate([{$project: {a: 1}}], {
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
    });
}, 12849400);

MongoRunner.stopMongod(conn);

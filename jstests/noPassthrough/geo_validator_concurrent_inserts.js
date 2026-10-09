/**
 * Regression test for concurrent evaluation of a shared collection validator using geospatial
 * operations.
 *
 * A validator's parsed MatchExpression (and the S2 geometry it owns) is shared across
 * all concurrent writes. S2Loop mutates internal lazy state from const query methods
 * (the S2EdgeIndex counters, and S2Loop::FindVertex's vertex_to_index_ map), so
 * concurrent inserts that evaluate the same validator could race on that state.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";

const kThreads = 8;
const kInsertsPerThread = 100;
const kQueryVertices = 3000;

function makeRing(vertexCount, radiusDeg, phase) {
    const coords = [];
    for (let i = 0; i < vertexCount; i++) {
        const angle = phase + (2 * Math.PI * i) / vertexCount;
        coords.push([radiusDeg * Math.cos(angle), radiusDeg * Math.sin(angle)]);
    }
    coords.push(coords[0]);
    return coords;
}

const conn = MongoRunner.runMongod();
assert.neq(null, conn);

const db = conn.getDB("test");
const collName = "geo_validator_concurrent_inserts";
db[collName].drop();

// Plain (non-strict-winding) GeoJSON polygon validator. Deliberately more vertices than
// the inserted documents so the shared validator loop is the larger loop during
// intersection checks. Its edge index is pre-built at parse time; only the lazily-built
// FindVertex map remains to be mutated at insert time.
assert.commandWorked(
    db.createCollection(collName, {
        validator: {
            loc: {
                $geoIntersects: {
                    $geometry: {type: "Polygon", coordinates: [makeRing(kQueryVertices, 1.0, 0.0)]},
                },
            },
        },
        validationAction: "error",
    }),
);

const smallPoly = {
    type: "Polygon",
    coordinates: [makeRing(6, 0.0001, 0.0)],
};
const point = {
    type: "Point",
    coordinates: [0.0, 0.0],
};

const host = conn.host;

let threads = [];
for (let t = 0; t < kThreads; t++) {
    // Alternate geometry types so both the polygon and point intersection paths are hit.
    const geom = t % 2 === 0 ? smallPoly : point;
    const thread = new Thread(
        function (host, dbName, collName, geom, threadId, inserts) {
            const conn = new Mongo(host);
            const coll = conn.getDB(dbName)[collName];
            for (let i = 0; i < inserts; i++) {
                assert.commandWorked(
                    coll.insertOne({
                        _id: threadId * 1000000 + i,
                        loc: geom,
                    }),
                );
            }
            conn.close();
        },
        host,
        db.getName(),
        collName,
        geom,
        t,
        kInsertsPerThread,
    );
    threads.push(thread);
    thread.start();
}

for (const thread of threads) {
    thread.join();
}

// The server must still be responsive after all concurrent validations.
assert.commandWorked(db.adminCommand({ping: 1}));

MongoRunner.stopMongod(conn);

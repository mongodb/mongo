/**
 * Tests that a standalone mongod is able to pin code segments on startup when
 * 'lockCodeSegmentsInMemory=true'.
 * @tags: [incompatible_with_macos, incompatible_with_windows_tls]
 */

(function() {
"use strict";

const conn = MongoRunner.runMongod({setParameter: {lockCodeSegmentsInMemory: true}});
assert.neq(null, conn, "mongod was unable to start up");
assert.eq(0, MongoRunner.stopMongod(conn));
}());

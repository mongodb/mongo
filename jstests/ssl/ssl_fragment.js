/**
 * Test that a large request and response works correctly.
 */
(function() {
    'use strict';

    var conn = MongoRunner.runMongod({
        sslMode: "requireSSL",
        sslPEMKeyFile: "jstests/libs/server.pem",
    });

    var large = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    var s = large;

    // SSL packets have a max size of ~16 kb so to test packet fragmentation support, create a
    // string larger then 16kb.
    for (let i = 0; i < 5 * 1700; i++) {
        s += large;
    }

    let ssl_frag = conn.getCollection('test.ssl_frag');
    assert.writeOK(ssl_frag.insert({_id: large}));

    let docs = ssl_frag.find({});
    assert.lt(2 * 16 * 1024, Object.bsonsize(docs), "test doc too small");

    MongoRunner.stopMongod(conn);
})();

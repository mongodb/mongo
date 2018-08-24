/**
 * Test that a large request and response works correctly.
 */
(function() {
    'use strict';

    const conn = MongoRunner.runMongod({
        sslMode: "requireSSL",
        sslPEMKeyFile: "jstests/libs/server.pem",
        networkMessageCompressors: 'disabled',
    });

    // SSL packets have a max size of ~16 kb so to test packet fragmentation support, create a
    // string larger then 16kb.
    const chunk = 'E$%G^56w4v5v54Vv$V@#t2#%t56u7B$ub%6 NU@ Y3qv4Yq%yq4C%yx$%zh';  // random data
    let s = '';
    while (s.length < (8 * 1024 * 1024)) {
        s += chunk;
    }

    const ssl_frag = conn.getCollection('test.ssl_frag');
    assert.writeOK(ssl_frag.insert({_id: "large_str", foo: s}));

    const read = ssl_frag.find({_id: "large_str"}).toArray()[0].foo;
    assert.eq(s, read, "Did not receive value written");

    MongoRunner.stopMongod(conn);
})();

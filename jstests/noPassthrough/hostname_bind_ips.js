// Check that connecting via IPv4 keeps working when
// binding to localhost and enabling IPv6.

(function() {
    'use strict';

    const proc = MerizoRunner.runMerizod({bind_ip: "localhost", "ipv6": ""});
    assert.neq(proc, null);

    assert.soon(function() {
        try {
            const uri = 'merizodb://127.0.0.1:' + proc.port + '/test';
            const conn = new Merizo(uri);
            assert.commandWorked(conn.adminCommand({ping: 1}));
            return true;
        } catch (e) {
            return false;
        }
    }, "Cannot connect to 127.0.0.1 when bound to localhost", 30 * 1000);
    MerizoRunner.stopMerizod(proc);
})();

// Attempt to verify that connections can make use of TCP_FASTOPEN

(function() {
'use strict';

// Does it make sense to expect TFO support?
try {
    // Both client and server bits must be set to run this test.
    const val = cat("/proc/sys/net/ipv4/tcp_fastopen");
    if ((Number.parseInt(val) & 3) != 3) {
        print("==Skipping test, tcp_fastopen not enabled: " + val);
        return;
    }
} catch (e) {
    // File not found or unreadable, assume no TFO support.
    print("==Skipping test, unable to read /proc/sys/net/ipv4/tcp_fastopen");
    return;
}

const initial = db.serverStatus().network.tcpFastOpen;
printjson(initial);

const confused = "proc file suggests this kernel is capable, but setsockopt failed";
assert.eq(true, initial.serverSupported, confused);
assert.eq(true, initial.clientSupported, confused);

// Initial connect to be sure a TFO cookie is requested and received.
const netConn1 = runMongoProgram('mongo', '--port', myPort(), '--eval', ';');
assert.eq(0, netConn1);

const first = db.serverStatus().network.tcpFastOpen;
printjson(first);

// Second connect using the TFO cookie.
const netConn2 = runMongoProgram('mongo', '--port', myPort(), '--eval', ';');
assert.eq(0, netConn2);

const second = db.serverStatus().network.tcpFastOpen;
printjson(second);

assert.gt(second.accepted, first.accepted, "Second connection did not trigger TFO");
})();

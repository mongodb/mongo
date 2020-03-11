// Attempt to verify that connections can make use of TCP_FASTOPEN
// @tags: [multiversion_incompatible, does_not_support_stepdowns]

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
print("/proc/net/netstat:");
print(cat("/proc/net/netstat"));

if (!initial.serverSupported || !initial.clientSupported) {
    print("==Skipping test, one or both setsockopt() calls failed");
    return;
}

function tryShell() {
    const conn = runMongoProgram('mongo', '--port', myPort(), '--eval', ';');
    print("/proc/net/netstat:");
    print(cat("/proc/net/netstat"));
    assert.eq(0, conn);
}

// Initial connect to be sure a TFO cookie is requested and received.
tryShell();

const first = db.serverStatus().network.tcpFastOpen;
printjson(first);

// Second connect using the TFO cookie.
tryShell();

const second = db.serverStatus().network.tcpFastOpen;
printjson(second);

assert.gt(second.accepted, first.accepted, "Second connection did not trigger TFO");
})();

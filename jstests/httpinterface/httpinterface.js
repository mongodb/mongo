// SERVER-9137 test that the httpinterface parameter enables the web interface
var conn = BongoRunner.runBongod({smallfiles: ""});
var httpPort = conn.port + 1000;

tryHttp = function() {
    try {
        var bongo = new Bongo('localhost:' + httpPort);
    } catch (e) {
        return false;
    }
    // if we managed to start and connect a new bongo then the web interface is working
    return true;
};

assert.throws(function() {
    assert.soon(tryHttp, "tryHttp failed, like we expected it to");
});

BongoRunner.stopBongod(conn);

conn = BongoRunner.runBongod({port: conn.port, smallfiles: "", httpinterface: ""});
assert.soon(tryHttp, "the web interface should be running on " + httpPort);

BongoRunner.stopBongod(conn);

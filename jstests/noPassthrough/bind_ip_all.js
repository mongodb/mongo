// Startup with --bind_ip_all should override net.bindIp and vice versa.

(function() {
'use strict';

const port = allocatePort();
const BINDIP = 'jstests/noPassthrough/libs/net.bindIp_localhost.yaml';
const BINDIPALL = 'jstests/noPassthrough/libs/net.bindIpAll.yaml';

function runTest(config, opt, expectStar, expectLocalhost) {
    clearRawMongoProgramOutput();
    const mongod =
        runMongoProgram('./mongod', '--port', port, '--config', config, opt, '--outputConfig');
    assert.eq(mongod, 0);
    const output = rawMongoProgramOutput();
    assert.eq(output.search(/bindIp: "\*"/) >= 0, expectStar, output);
    assert.eq(output.search(/bindIp: localhost/) >= 0, expectLocalhost, output);
    assert.eq(output.search(/bindIpAll:/) >= 0, false, output);
}

runTest(BINDIP, '--bind_ip_all', true, false);
runTest(BINDIPALL, '--bind_ip=localhost', false, true);
}());

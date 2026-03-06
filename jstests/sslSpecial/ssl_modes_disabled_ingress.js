// Test that tlsMode="disabled" is correctly enforced on
// ingress connections accepted by both TCP/IP and Unix Domain sockets.

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CA_CERT, CLIENT_CERT, runTLSModeTest} from "jstests/ssl/libs/ssl_helpers.js";

if (_isWindows()) {
    quit();
}

// jstests/ssl_linear/ssl_modes_not_disabled_ingress.js covers '{require|allow|prefer}TLS'. We can't
// include all of them in one file because ssl_special doesn't support requireTLS while
// ssl_linear doesn't support disabled.
const socketPrefix = `${MongoRunner.dataDir}/socketdir`;
mkdir(socketPrefix);
{
    const disabledTLSMode = {
        tlsMode: "disabled",
        unixSocketPrefix: socketPrefix,
    };

    const mongod = MongoRunner.runMongod(disabledTLSMode);
    runTLSModeTest(mongod, "disabled", CLIENT_CERT, CA_CERT, socketPrefix);
    MongoRunner.stopMongod(mongod);

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            keyFile: "jstests/libs/key1",
            configOptions: disabledTLSMode,
            mongosOptions: disabledTLSMode,
            rsOptions: disabledTLSMode,
            useHostname: false,
        },
    });
    runTLSModeTest(st.s0, "disabled", CLIENT_CERT, CA_CERT, socketPrefix);
    st.stop();
}

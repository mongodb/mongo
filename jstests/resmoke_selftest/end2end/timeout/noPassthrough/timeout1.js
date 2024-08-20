import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

// Signal that the test has started running
var sentinelPath = (_getEnv("TMPDIR") || _getEnv("TMP_DIR") || "/tmp") + "/timeout1.js.sentinel";
removeFile(sentinelPath);
writeFile(sentinelPath, "");

// Loop infinitely to simulate timeout.
while (true) {
    print('looping...');
    sleep(100);
}

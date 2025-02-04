import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

// Signal that the test has started running
var sentinelPath =
    (_getEnv("TMPDIR") || _getEnv("TMP_DIR") || "/tmp") + "/inner_level_timeout.js.sentinel";
removeFile(sentinelPath);

while (true) {
    print("looping");
    sleep(1000);
}

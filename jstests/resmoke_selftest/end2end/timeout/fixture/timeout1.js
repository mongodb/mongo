// Signal that the test has started running
let sentinelPath = (_getEnv("TMPDIR") || _getEnv("TMP_DIR") || "/tmp") + "/timeout0.js.sentinel";
removeFile(sentinelPath);

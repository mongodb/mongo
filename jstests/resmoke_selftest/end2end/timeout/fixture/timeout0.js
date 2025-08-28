// Signal that the test has started running
let sentinelPath = (_getEnv("TMPDIR") || _getEnv("TMP_DIR") || "/tmp") + "/timeout0.js.sentinel";
removeFile(sentinelPath);

// Loop infinitely to simulate timeout.
while (true) {
    print("looping...");
    sleep(100);
}

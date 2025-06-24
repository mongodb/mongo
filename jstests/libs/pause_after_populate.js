// If resmoke has been run with --pauseAfterPopulate, this function will sleep indefinitely.
export const checkPauseAfterPopulate = function() {
    if (TestData.pauseAfterPopulate) {
        jsTestLog("TestData.pauseAfterPopulate is set. Pausing indefinitely ...");
        sleep(2147483647);
    }
};

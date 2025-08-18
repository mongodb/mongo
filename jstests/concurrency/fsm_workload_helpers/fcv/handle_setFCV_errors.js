// Helper function for common accepted FCV errors in concurrency tests

export function handleRandomSetFCVErrors(e, targetFCV) {
    if (e.code === 5147403) {
        // Invalid fcv transition (e.g lastContinuous -> lastLTS)
        jsTestLog('setFCV: Invalid transition');
        return true;
    }
    if (e.code === 7428200) {
        // Cannot upgrade FCV if a previous FCV downgrade stopped in the middle of cleaning
        // up internal server metadata.
        assert.eq(latestFCV, targetFCV);
        jsTestLog(
            'setFCV: Cannot upgrade FCV if a previous FCV downgrade stopped in the middle \
            of cleaning up internal server metadata');
        return true;
    }
    if (e.code === 10778001) {
        // Cannot downgrade FCV if a previous FCV upgrade stopped in the middle of cleaning
        // up internal server metadata.
        assert(targetFCV === lastLTSFCV || targetFCV == lastContinuousFCV);
        jsTestLog(
            'setFCV: Cannot downgrade FCV if a previous FCV upgrade stopped in the middle \
            of cleaning up internal server metadata');
        return true;
    }
    if (e.code === 12587) {
        // Cannot downgrade FCV that requires a collMod command when index builds are
        // concurrently taking place.
        jsTestLog(
            'setFCV: Cannot downgrade the FCV that requires a collMod command when index \
            builds are concurrently running');
        return true;
    }
    return false;  // Error was not handled here; let the caller decide next steps.
}

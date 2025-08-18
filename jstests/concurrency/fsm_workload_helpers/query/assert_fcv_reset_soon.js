// Error codes needed for FCV reset logic
const kAcceptedSetFCVErrors = [
    // If setFCV failed due to collections in non primary shard (regardless of the best effort),
    // allow the state to run again eventually.
    ErrorCodes.CannotDowngrade,
    // Invalid fcv transition (e.g lastContinuous -> lastLTS).
    5147403,
    // Cannot upgrade FCV if a previous FCV downgrade stopped in the middle of cleaning up
    // internal server metadata.
    7428200,
    // Cannot downgrade FCV if a previous FCV upgrade stopped in the middle of cleaning up
    // internal server metadata.
    10778001,
    // Cannot downgrade FCV that requires a collMod command when index builds are concurrently
    // taking place.
    12587,
];

export function assertSetFCVSoon(db, FCV) {
    assert.soon(() => {
        let res = db.adminCommand({setFeatureCompatibilityVersion: FCV, confirm: true});

        if (res.ok) {
            return true;
        } else if (!kAcceptedSetFCVErrors.includes(res.code)) {
            assert.commandWorked(res);
        }
        jsTestLog('retrying setFeatureCompatibilityVersion that failed with: ' + tojson(res));
        return false;
    });
}

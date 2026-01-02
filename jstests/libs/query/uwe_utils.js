/**
 * Checks whether the unified write executor is used for sharded writes.
 */
export function isUweEnabled(db) {
    const UWEFlagName = "featureFlagUnifiedWriteExecutor";
    // Check the value directly on mongos if the flag exists. This is necessary because we need to
    // check the value on the router that we are targeting, but 'FeatureFlagUtil' will check the
    // value on mongod instead.
    const res = db.adminCommand({getParameter: 1, [UWEFlagName]: 1});
    if (res.code === ErrorCodes.InvalidOptions) {
        return false;
    }
    assert(res.hasOwnProperty(UWEFlagName), res);
    return res[UWEFlagName].value === true;
}

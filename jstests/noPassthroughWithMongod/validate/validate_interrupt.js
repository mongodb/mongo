let t = db.validate_interrupt;
t.drop();

let bulk = t.initializeUnorderedBulkOp();

let i;
for (i = 0; i < 1000; i++) {
    bulk.insert({a: i});
}
assert.commandWorked(bulk.execute());

function setTimeoutFailPoint(mode) {
    let res = db.adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: mode});
    assert.commandWorked(res);
}

setTimeoutFailPoint("alwaysOn");
let res = t.runCommand({validate: t.getName(), full: true, maxTimeMS: 1});
setTimeoutFailPoint("off");

// Sanity check to make sure the failpoint is turned off.
assert.commandWorked(t.runCommand({validate: t.getName(), full: true}));

if (res.ok === 0) {
    assert.eq(res.code, ErrorCodes.MaxTimeMSExpired, "validate command did not time out:\n" + tojson(res));
} else {
    // validate() should only succeed if it EBUSY'd. See SERVER-23131.
    let numWarnings = res.warnings.length;
    // validate() could EBUSY when verifying the index and/or the RecordStore, so EBUSY could
    // appear once or twice.
    assert(numWarnings === 1 || numWarnings === 2, "Expected 1 or 2 validation warnings:\n" + tojson(res));
    assert(res.warnings[0].includes("EBUSY"), "Expected an EBUSY warning:\n" + tojson(res));
    if (numWarnings === 2) {
        assert(res.warnings[1].includes("EBUSY"), "Expected an EBUSY warning:\n" + tojson(res));
    }
}

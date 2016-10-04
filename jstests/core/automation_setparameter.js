// Tests that using setParameter to update the parameter 'automationServiceDescriptor' causes a
// field with that name to be echoed back in isMaster. See SERVER-18399 for more details.

(function() {

    // Run isMaster, and if it contains an automation service descriptor, save it, so we can restore
    // it later. If it wasn't set, original will just be undefined.
    var res = assert.commandWorked(db.runCommand({isMaster: 1}));
    var original = res.automationServiceDescriptor;

    // Try to set the descriptor to an invalid value: only strings are supported.
    assert.commandFailedWithCode(db.adminCommand({setParameter: 1, automationServiceDescriptor: 0}),
                                 ErrorCodes.TypeMismatch);

    // Try to set the descriptor to an invalid value: Only 64 characters are allowed.
    assert.commandFailedWithCode(db.adminCommand({
        setParameter: 1,
        automationServiceDescriptor:
            "1234567812345678123456781234567812345678123456781234567812345678X"
    }),
                                 ErrorCodes.Overflow);

    // Short strings are okay.
    res = assert.commandWorked(
        db.adminCommand({setParameter: 1, automationServiceDescriptor: "some_service"}));

    // Verify that the setParameter 'was' field contains what we expected.
    if (original)
        assert.eq(original, res.was);

    // Verify that the 'some_service' string is now echoed back to us in isMaster
    res = assert.commandWorked(db.runCommand({isMaster: 1}));
    assert.eq(res.automationServiceDescriptor, "some_service");

    // Verify that setting the descriptor to the empty string is ok, and prevents it from being
    // echoed back
    assert.commandWorked(db.adminCommand({setParameter: 1, automationServiceDescriptor: ""}));
    res = assert.commandWorked(db.runCommand({isMaster: 1}));
    assert(!res.hasOwnProperty('automationServiceDescriptor'));

    // Verify that the shell has the correct prompt.
    var originalPrompt = db.getMongo().promptPrefix;
    assert.commandWorked(db.adminCommand({setParameter: 1, automationServiceDescriptor: "set"}));
    db.getMongo().promptPrefix = undefined;
    assert(/\[automated\]/.test(defaultPrompt()));

    // Restore whatever was there originally.
    if (!original)
        original = "";
    assert.commandWorked(db.adminCommand({setParameter: 1, automationServiceDescriptor: original}));
    db.getMongo().promptPrefix = originalPrompt;
}());

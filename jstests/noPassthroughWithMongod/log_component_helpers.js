// Basic sanity check of log component helpers

(function(db) {
    "use strict";
    var merizo = db.getMerizo();

    // Get current log component setttings. We will reset to these later.
    var originalSettings =
        assert.commandWorked(db.adminCommand({getParameter: 1, logComponentVerbosity: 1}))
            .logComponentVerbosity;

    // getLogComponents
    var components1 = merizo.getLogComponents();
    assert.docEq(components1, originalSettings);

    // getLogComponents via db
    var components2 = db.getLogComponents();
    assert.docEq(components2, originalSettings);

    // setLogLevel - default component
    merizo.setLogLevel(2);
    assert.eq(merizo.getLogComponents().verbosity, 2);

    db.setLogLevel(0);
    assert.eq(merizo.getLogComponents().verbosity, 0);

    // setLogLevel - valid log component
    merizo.setLogLevel(2, "storage.journal");
    assert.eq(merizo.getLogComponents().storage.journal.verbosity, 2);

    db.setLogLevel(1, "storage.journal");
    assert.eq(merizo.getLogComponents().storage.journal.verbosity, 1);

    // setLogLevel - invalid argument
    assert.throws(function() {
        merizo.setLogLevel(2, 24);
    });
    assert.throws(function() {
        db.setLogLevel(2, ["array", "not.allowed"]);
    });

    // Restore originalSettings
    assert.commandWorked(
        db.adminCommand({setParameter: 1, logComponentVerbosity: originalSettings}));
}(db));

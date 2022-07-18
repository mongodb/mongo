// @tags: [
//   assumes_superuser_permissions,
//   does_not_support_stepdowns,
//   requires_fcv_61,
//   # This test attempts to compare the response from running the {getParameter: "*"}
//   # command multiple times, which may observe the change to the failpoint enabled by the
//   # migration hook.
//   tenant_migration_incompatible,
// ]

// Tests for accessing logLevel server parameter using getParameter/setParameter commands
// and shell helpers.

(function() {
'use strict';

function scrub(obj) {
    delete obj["operationTime"];
    delete obj["$clusterTime"];
    delete obj["lastCommittedOpTime"];
    // There are Failpoint manipulations in concurrent tasks in the jstest
    // environment. So scrub the volatile "failpoint." parameters.
    for (let key in obj)
        if (key.startsWith("failpoint."))
            delete obj[key];
    return obj;
}

const old = scrub(assert.commandWorked(db.adminCommand({"getParameter": "*"})));
// the first time getParameter sends a request to with a shardingTaskExecutor and this sets an
// operationTime. The following commands do not use shardingTaskExecutor.
const tmp1 = assert.commandWorked(db.adminCommand({"setParameter": 1, "logLevel": 5}));
const tmp2 = assert.commandWorked(db.adminCommand({"setParameter": 1, "logLevel": old.logLevel}));
const now = scrub(assert.commandWorked(db.adminCommand({"getParameter": "*"})));

assert.eq(old, now, "A");
assert.eq(old.logLevel, tmp1.was, "B");
assert.eq(5, tmp2.was, "C");

//
// component verbosity
//

// verbosity for log component hierarchy
printjson(old.logComponentVerbosity);
assert.neq(undefined, old.logComponentVerbosity, "log component verbosity not available");
assert.eq(old.logLevel,
          old.logComponentVerbosity.verbosity,
          "default component verbosity should match logLevel");
assert.neq(undefined,
           old.logComponentVerbosity.storage.journal.verbosity,
           "journal verbosity not available");

// Non-object log component verbosity should be rejected.
assert.commandFailed(db.adminCommand({"setParameter": 1, logComponentVerbosity: "not an object"}));

// Non-numeric verbosity for component should be rejected.
assert.commandFailed(db.adminCommand(
    {"setParameter": 1, logComponentVerbosity: {storage: {journal: {verbosity: "not a number"}}}}));

// Invalid component shall be rejected
assert.commandFailed(
    db.adminCommand({"setParameter": 1, logComponentVerbosity: {NoSuchComponent: {verbosity: 2}}}));

// Set multiple component log levels at once.
(function() {
assert.commandWorked(db.adminCommand({
    "setParameter": 1,
    logComponentVerbosity: {
        verbosity: 2,
        accessControl: {verbosity: 0},
        storage: {verbosity: 3, journal: {verbosity: 5}}
    }
}));

const result = assert.commandWorked(db.adminCommand({"getParameter": 1, logComponentVerbosity: 1}))
                   .logComponentVerbosity;

assert.eq(2, result.verbosity);
assert.eq(0, result.accessControl.verbosity);
assert.eq(3, result.storage.verbosity);
assert.eq(5, result.storage.journal.verbosity);
})();

// Set multiple component log levels at once.
// Unrecognized field names not mapping to a log component shall be rejected
// No changes shall apply.
(function() {
assert.commandFailed(db.adminCommand({
    "setParameter": 1,
    logComponentVerbosity: {
        verbosity: 6,
        accessControl: {verbosity: 5},
        storage: {verbosity: 4, journal: {verbosity: 6}},
        NoSuchComponent: {verbosity: 2},
        extraField: 123
    }
}));

const result = assert.commandWorked(db.adminCommand({"getParameter": 1, logComponentVerbosity: 1}))
                   .logComponentVerbosity;

assert.eq(2, result.verbosity);
assert.eq(0, result.accessControl.verbosity);
assert.eq(3, result.storage.verbosity);
assert.eq(5, result.storage.journal.verbosity);
})();

// Clear verbosity for default and journal.
(function() {
assert.commandWorked(db.adminCommand({
    "setParameter": 1,
    logComponentVerbosity: {verbosity: -1, storage: {journal: {verbosity: -1}}}
}));

const result = assert.commandWorked(db.adminCommand({"getParameter": 1, logComponentVerbosity: 1}))
                   .logComponentVerbosity;

assert.eq(0, result.verbosity);
assert.eq(0, result.accessControl.verbosity);
assert.eq(3, result.storage.verbosity);
assert.eq(-1, result.storage.journal.verbosity);
})();

// Set accessControl verbosity using numerical level instead of
// subdocument with 'verbosity' field.
(function() {
assert.commandWorked(
    db.adminCommand({"setParameter": 1, logComponentVerbosity: {accessControl: 5}}));

const result = assert.commandWorked(db.adminCommand({"getParameter": 1, logComponentVerbosity: 1}))
                   .logComponentVerbosity;

assert.eq(5, result.accessControl.verbosity);
})();

// Restore old verbosity values.
assert.commandWorked(
    db.adminCommand({"setParameter": 1, logComponentVerbosity: old.logComponentVerbosity}));

// Checks server parameter for redaction of encrypted fields in BSON Objects.
assert(old.hasOwnProperty('redactEncryptedFields'),
       'server parameter for toggling bindata 6 redaction not available: ' + tojson(old));
assert.commandWorked(db.adminCommand({"setParameter": 1, redactEncryptedFields: false}));
const result = assert.commandWorked(db.adminCommand({"getParameter": 1, redactEncryptedFields: 1}))
                   .redactEncryptedFields;
assert(!result,
       'setParameter worked on redaction setting but getParameter returned unexpected result.');
assert.commandWorked(
    db.adminCommand({"setParameter": 1, redactEncryptedFields: old.redactEncryptedFields}));

const isMongos = (db.hello().msg === 'isdbgrid');
if (!isMongos) {
    //
    // oplogFetcherSteadyStateMaxFetcherRestarts
    //
    let origRestarts = assert
                           .commandWorked(db.adminCommand(
                               {getParameter: 1, oplogFetcherSteadyStateMaxFetcherRestarts: 1}))
                           .oplogFetcherSteadyStateMaxFetcherRestarts;
    assert.gte(origRestarts,
               0,
               'default value of oplogFetcherSteadyStateMaxFetcherRestarts cannot be negative');
    assert.commandFailedWithCode(
        db.adminCommand({setParameter: 1, oplogFetcherSteadyStateMaxFetcherRestarts: -1}),
        ErrorCodes.BadValue,
        'server should reject negative values for oplogFetcherSteadyStateMaxFetcherRestarts');
    assert.commandWorked(
        db.adminCommand({setParameter: 1, oplogFetcherSteadyStateMaxFetcherRestarts: 0}));
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, oplogFetcherSteadyStateMaxFetcherRestarts: origRestarts + 20}));
    assert.eq(origRestarts + 20,
              assert
                  .commandWorked(db.adminCommand(
                      {getParameter: 1, oplogFetcherSteadyStateMaxFetcherRestarts: 1}))
                  .oplogFetcherSteadyStateMaxFetcherRestarts);
    // Restore original value.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, oplogFetcherSteadyStateMaxFetcherRestarts: origRestarts}));

    //
    // oplogFetcherInitialSyncStateMaxFetcherRestarts
    //
    origRestarts = assert
                       .commandWorked(db.adminCommand(
                           {getParameter: 1, oplogFetcherInitialSyncMaxFetcherRestarts: 1}))
                       .oplogFetcherInitialSyncMaxFetcherRestarts;
    assert.gte(origRestarts,
               0,
               'default value of oplogFetcherSteadyStateMaxFetcherRestarts cannot be negative');
    assert.commandFailedWithCode(
        db.adminCommand({setParameter: 1, oplogFetcherInitialSyncMaxFetcherRestarts: -1}),
        ErrorCodes.BadValue,
        'server should reject negative values for oplogFetcherSteadyStateMaxFetcherRestarts');
    assert.commandWorked(
        db.adminCommand({setParameter: 1, oplogFetcherInitialSyncMaxFetcherRestarts: 0}));
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, oplogFetcherInitialSyncMaxFetcherRestarts: origRestarts + 20}));
    assert.eq(origRestarts + 20,
              assert
                  .commandWorked(db.adminCommand(
                      {getParameter: 1, oplogFetcherInitialSyncMaxFetcherRestarts: 1}))
                  .oplogFetcherInitialSyncMaxFetcherRestarts);
    // Restore original value.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, oplogFetcherInitialSyncMaxFetcherRestarts: origRestarts}));
}
})();

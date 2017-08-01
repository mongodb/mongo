// Provides convenience methods for confirming system.profile content.

// Retrieve latest system.profile entry.
function getLatestProfilerEntry(profileDB, filter) {
    if (filter === null) {
        filter = {};
    }
    var cursor = profileDB.system.profile.find(filter).sort({$natural: -1});
    assert(
        cursor.hasNext(),
        "could not find any entries in the profile collection matching filter: " + tojson(filter));
    return cursor.next();
}

// Returns a string representing the wire protocol used for commands run on the given connection.
// This string matches the system.profile "protocol" field when commands are profiled.
function getProfilerProtocolStringForCommand(conn) {
    const protocols = conn.getClientRPCProtocols();
    if ("all" === protocols || /Msg/.test(protocols))
        return "op_msg";
    if (/Command/.test(protocols))
        return "op_command";
    if (/Query/.test(protocols))
        return "op_query";
    doassert(`Unknown prototocol string ${protocols}`);
}

/**
 * Throws an assertion if the profiler contains more than 'maxExpectedMatches' entries matching
 * "filter", or if there are no matches. Optional arguments "errorMsgFilter" and "errorMsgProj"
 * limit profiler output if this asserts.
 */
function profilerHasAtLeastOneAtMostNumMatchingEntriesOrThrow(
    {profileDB, filter, maxExpectedMatches, errorMsgFilter, errorMsgProj}) {
    assert(typeof maxExpectedMatches === 'number' && maxExpectedMatches > 0,
           "'maxExpectedMatches' must be a number > 0");

    const numMatches = profileDB.system.profile.find(filter).itcount();

    assert.gt(numMatches,
              0,
              "Expected at least 1 op matching: " + tojson(filter) + " in profiler " +
                  tojson(profileDB.system.profile.find(errorMsgFilter, errorMsgProj).toArray()));

    assert.lte(numMatches,
               maxExpectedMatches,
               "Expected at most " + maxExpectedMatches + " op(s) matching: " + tojson(filter) +
                   " in profiler " +
                   tojson(profileDB.system.profile.find(errorMsgFilter, errorMsgProj).toArray()));
}

/**
 * Throws an assertion if the profiler does not contain exactly 'numExpectedMatches' entries
 * matching "filter". Optional arguments "errorMsgFilter" and "errorMsgProj" limit profiler output
 * if this asserts.
 */
function profilerHasNumMatchingEntriesOrThrow(
    {profileDB, filter, numExpectedMatches, errorMsgFilter, errorMsgProj}) {
    assert(typeof numExpectedMatches === 'number' && numExpectedMatches >= 0,
           "'numExpectedMatches' must be a number >= 0");

    assert.eq(profileDB.system.profile.find(filter).itcount(),
              numExpectedMatches,
              "Expected exactly " + numExpectedMatches + " op(s) matching: " + tojson(filter) +
                  " in profiler " +
                  tojson(profileDB.system.profile.find(errorMsgFilter, errorMsgProj).toArray()));
}

/**
 * Throws an assertion if the profiler does not contain any entries matching "filter". Optional
 * arguments "errorMsgFilter" and "errorMsgProj" limit profiler output if this asserts.
 */
function profilerHasAtLeastOneMatchingEntryOrThrow(
    {profileDB, filter, errorMsgFilter, errorMsgProj}) {
    assert.gte(profileDB.system.profile.find(filter).itcount(),
               1,
               "Expected at least 1 op matching: " + tojson(filter) + " in profiler " +
                   tojson(profileDB.system.profile.find(errorMsgFilter, errorMsgProj).toArray()));
}

/**
 * Throws an assertion if the profiler does not contain exactly one entry matching "filter".
 * Optional arguments "errorMsgFilter" and "errorMsgProj" limit profiler output if this asserts.
 */
function profilerHasSingleMatchingEntryOrThrow({profileDB, filter, errorMsgFilter, errorMsgProj}) {
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: profileDB,
        filter: filter,
        numExpectedMatches: 1,
        errorMsgFilter: errorMsgFilter,
        errorMsgProj: errorMsgProj
    });
}

/**
 * Throws an assertion if the profiler contains any entries matching "filter". Optional arguments
 * "errorMsgFilter" and "errorMsgProj" limit profiler output if this asserts.
 */
function profilerHasZeroMatchingEntriesOrThrow({profileDB, filter, errorMsgFilter, errorMsgProj}) {
    profilerHasNumMatchingEntriesOrThrow({
        profileDB: profileDB,
        filter: filter,
        numExpectedMatches: 0,
        errorMsgFilter: errorMsgFilter,
        errorMsgProj: errorMsgProj
    });
}

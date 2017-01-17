// Provides convenience methods for confirming system.profile content.

// Retrieve latest system.profile entry.
function getLatestProfilerEntry(inputDb, filter) {
    var cursor = inputDb.system.profile.find(filter === null ? {} : filter);
    return cursor.sort({$natural: -1}).next();
}

// Returns a string representing the wire protocol used for commands run on the given connection.
// This string matches the system.profile "protocol" field when commands are profiled.
function getProfilerProtocolStringForCommand(conn) {
    if ("opQueryOnly" === conn.getClientRPCProtocols()) {
        return "op_query";
    }

    return "op_command";
}

// Throws an assertion if the profiler does not contain exactly one entry matching <filter>.
// Optional arguments <errorMsgFilter> and <errorMsgProj> limit profiler output if this asserts.
function profilerHasSingleMatchingEntryOrThrow(inputDb, filter, errorMsgFilter, errorMsgProj) {
    assert.eq(inputDb.system.profile.find(filter).itcount(),
              1,
              "Expected exactly one op matching: " + tojson(filter) + " in profiler " +
                  tojson(inputDb.system.profile.find(errorMsgFilter, errorMsgProj).toArray()));
}
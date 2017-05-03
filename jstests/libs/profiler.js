// Provides convenience methods for confirming system.profile content.

// Retrieve latest system.profile entry.
function getLatestProfilerEntry(inputDb, filter) {
    if (filter === null) {
        filter = {};
    }
    var cursor = inputDb.system.profile.find(filter).sort({$natural: -1});
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

// Throws an assertion if the profiler does not contain exactly one entry matching <filter>.
// Optional arguments <errorMsgFilter> and <errorMsgProj> limit profiler output if this asserts.
function profilerHasSingleMatchingEntryOrThrow(inputDb, filter, errorMsgFilter, errorMsgProj) {
    assert.eq(inputDb.system.profile.find(filter).itcount(),
              1,
              "Expected exactly one op matching: " + tojson(filter) + " in profiler " +
                  tojson(inputDb.system.profile.find(errorMsgFilter, errorMsgProj).toArray()));
}

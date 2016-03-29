// Provides convenience methods for confirming system.profile content.

// Retrieve latest system.profile entry.
function getLatestProfilerEntry(inputDb) {
    var cursor = inputDb.system.profile.find();
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
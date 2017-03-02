// Regression test for SERVER-21673.

// A user can configure the shell to send commands via OP_QUERY or OP_COMMAND. This can be done at
// startup using the "--rpcProtocols" command line option, or at runtime using the
// "setClientRPCProtocols" method on the Bongo object.

var RPC_PROTOCOLS = {OP_QUERY: "opQueryOnly", OP_COMMAND: "opCommandOnly"};

(function() {
    "use strict";

    db.rpcProtocols.drop();

    var oldProfilingLevel = db.getProfilingLevel();

    assert.commandWorked(db.setProfilingLevel(2));

    function runInShell(rpcProtocol, func) {
        assert(0 == _runBongoProgram("bongo",
                                     "--rpcProtocols=" + rpcProtocol,
                                     "--readMode=commands",  // ensure we use the find command.
                                     "--eval",
                                     "(" + func.toString() + ")();",
                                     db.getBongo().host));
    }

    // Test that --rpcProtocols=opQueryOnly forces OP_QUERY commands.
    runInShell(RPC_PROTOCOLS.OP_QUERY, function() {
        assert(db.getBongo().getClientRPCProtocols() === "opQueryOnly");
        db.getSiblingDB("test").rpcProtocols.find().comment("opQueryCommandLine").itcount();
    });
    var profileDoc = db.system.profile.findOne({"query.comment": "opQueryCommandLine"});
    assert(profileDoc !== null);
    assert.eq(profileDoc.protocol, "op_query");

    // Test that --rpcProtocols=opCommandOnly forces OP_COMMAND commands.
    runInShell(RPC_PROTOCOLS.OP_COMMAND, function() {
        assert(db.getBongo().getClientRPCProtocols() === "opCommandOnly");
        db.getSiblingDB("test").rpcProtocols.find().comment("opCommandCommandLine").itcount();
    });
    profileDoc = db.system.profile.findOne({"query.comment": "opCommandCommandLine"});
    assert(profileDoc !== null);
    assert.eq(profileDoc.protocol, "op_command");

    // Test that .setClientRPCProtocols("opQueryOnly") forces OP_QUERY commands. We start the shell
    // in OP_COMMAND only mode, then switch it to OP_QUERY mode at runtime.
    runInShell(RPC_PROTOCOLS.OP_COMMAND, function() {
        assert(db.getBongo().getClientRPCProtocols() === "opCommandOnly");
        db.getBongo().setClientRPCProtocols("opQueryOnly");
        assert(db.getBongo().getClientRPCProtocols() === "opQueryOnly");
        db.getSiblingDB("test").rpcProtocols.find().comment("opQueryRuntime").itcount();
    });
    profileDoc = db.system.profile.findOne({"query.comment": "opQueryRuntime"});
    assert(profileDoc !== null);
    assert.eq(profileDoc.protocol, "op_query");

    // Test that .setClientRPCProtocols("opCommandOnly") forces OP_COMMAND commands. We start the
    // shell in OP_QUERY only mode, then switch it to OP_COMMAND mode at runtime.
    runInShell(RPC_PROTOCOLS.OP_QUERY, function() {
        assert(db.getBongo().getClientRPCProtocols() === "opQueryOnly");
        db.getBongo().setClientRPCProtocols("opCommandOnly");
        assert(db.getBongo().getClientRPCProtocols() === "opCommandOnly");
        db.getSiblingDB("test").rpcProtocols.find().comment("opCommandRuntime").itcount();
    });
    profileDoc = db.system.profile.findOne({"query.comment": "opCommandRuntime"});
    assert(profileDoc !== null);
    assert.eq(profileDoc.protocol, "op_command");

    // Reset profiling level.
    assert.commandWorked(db.setProfilingLevel(oldProfilingLevel));
})();

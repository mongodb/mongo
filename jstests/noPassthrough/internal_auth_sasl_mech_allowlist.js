/**
 * SERVER-130264: a forged intra-cluster peer that advertises only "PLAIN" in its hello reply must
 * not be able to downgrade an egress internal-auth connection to PLAIN (which would send the raw
 * keyfile in cleartext). The connecting node must filter saslSupportedMechs against a client-side
 * allowlist and never issue a PLAIN saslStart.
 *
 * The test stands up a keyfile-protected replica set and a fake peer (a Python script speaking just
 * enough of the wire protocol). The peer is added to the replica set config, so the primary opens
 * egress internal-auth connections to it (via replica set heartbeats). We assert the primary
 * attempts the handshake but never sends a saslStart to the forged peer.
 *
 * @tags: [requires_persistence]
 */
import {getPython3Binary} from "jstests/libs/python.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// useHostName:false so the existing member is referenced as 127.0.0.1; the forged peer also binds
// to 127.0.0.1 and a replica set config may not mix localhost and non-localhost host names.
const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1", useHostName: false});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

// keyFile enables authentication; create and log in as an admin user (via the localhost exception)
// so the shell can read and reconfigure the set.
const adminDB = primary.getDB("admin");
adminDB.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
assert(adminDB.auth("admin", "pwd"), "failed to authenticate as admin");

const setName = rst.name;

// Launch the forged peer, advertising the real set name so the primary keeps it in the topology and
// keeps opening (internal-auth) egress connections to it.
const peerPort = allocatePort();
const outfile = MongoRunner.dataPath + "forged_sasl_mech_peer_cmds.txt";
const peerPid = _startMongoProgram({
    args: [
        getPython3Binary(),
        "-u",
        "jstests/noPassthrough/libs/forged_sasl_mech_peer.py",
        "--port=" + peerPort,
        "--outfile=" + outfile,
        "--set-name=" + setName,
    ],
});
assert.soon(() => checkProgram(peerPid).alive, "forged peer failed to start");

const peerHost = "127.0.0.1:" + peerPort;

// Returns the list of "<name>\t<fullCommandDoc>" lines the forged peer has received so far.
const receivedLines = () => {
    try {
        return cat(outfile)
            .trim()
            .split("\n")
            .filter((line) => line.length > 0);
    } catch (e) {
        return [];
    }
};
const receivedCommandNames = () => receivedLines().map((line) => line.split("\t")[0]);

// Add the forged peer as a non-voting, zero-priority member so the primary heartbeats it (opening
// egress internal-auth connections) without letting it influence the set. A short heartbeat
// interval makes the primary retry connections quickly.
const config = rst.getReplSetConfigFromNode();
config.version++;
config.settings = Object.assign({}, config.settings, {heartbeatIntervalMillis: 500});
config.members.push({_id: 2, host: peerHost, priority: 0, votes: 0});
assert.commandWorked(primary.adminCommand({replSetReconfig: config, force: true}));

// The primary should attempt the hello handshake against the forged peer.
assert.soon(
    () => receivedCommandNames().includes("hello"),
    "primary never attempted a hello handshake against the forged peer",
);

// Give the primary ample time to (mis)behave. Without the allowlist fix, the egress internal-auth
// connection would fall back to a PLAIN saslStart against the forged peer once speculative SCRAM
// auth fails. With the fix, validateHost rejects the connection before authentication is attempted.
// We wait for a number of retried handshakes to be confident the auth step would have run.
assert.soon(
    () => receivedCommandNames().filter((c) => c === "hello").length >= 5,
    "expected the primary to retry the handshake against the forged peer",
    30 * 1000,
);
sleep(3000);

const lines = receivedLines();
jsTestLog("Commands received by forged peer:\n" + lines.join("\n"));

const commandNames = receivedCommandNames();
assert(
    !commandNames.includes("saslStart"),
    "primary issued a saslStart to the forged peer, indicating a PLAIN downgrade:\n" + lines.join("\n"),
);

stopMongoProgramByPid(peerPid);
rst.stopSet();

// Verify that clients can speculatively authenticate to mongos.
// @tags: [requires_sharding]

import {ShardingTest} from "jstests/libs/shardingtest.js";

const fallbackMech = "SCRAM-SHA-256";
const keyfile = "jstests/libs/key1";
const st = new ShardingTest({
    mongos: 1,
    keyFile: keyfile,
    other: {mongosOptions: {auth: null}, configOptions: {auth: null}, rsOptions: {auth: null}},
});

const admin = st.s.getDB("admin");
admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
admin.auth("admin", "pwd");

let lastStats = assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
jsTest.log("Inintial stats: " + lastStats);

const ingressMechs = ["SCRAM-SHA-1", "SCRAM-SHA-256", "MONGODB-X509"];

function test(uri, incrMech, isClusterAuth = false) {
    jsTest.log("Connecting to: " + uri);
    assert.eq(runMongoProgram("mongo", uri, "--eval", ";"), 0);

    const stats = assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
    try {
        assert.eq(Object.keys(lastStats).length, Object.keys(stats).length);
        Object.keys(lastStats).forEach(function (mech) {
            if (ingressMechs.includes(mech)) {
                const inc = mech === incrMech ? 1 : 0;
                const clusterInc = mech === incrMech && isClusterAuth ? 1 : 0;

                const specBefore = lastStats[mech].ingress.speculativeAuthenticate;
                const specAfter = stats[mech].ingress.speculativeAuthenticate;
                assert.eq(specAfter.total, specBefore.total + inc);
                assert.eq(specAfter.successful, specBefore.successful + inc);

                const clusterBefore = lastStats[mech].ingress.clusterAuthenticate;
                const clusterAfter = stats[mech].ingress.clusterAuthenticate;
                assert.eq(clusterAfter.total, clusterBefore.total + clusterInc);
                assert.eq(clusterAfter.successful, clusterBefore.successful + clusterInc);

                const allBefore = lastStats[mech].ingress.authenticate;
                const allAfter = stats[mech].ingress.authenticate;
                assert.eq(allAfter.total, allBefore.total + inc);
                assert.eq(allAfter.successful, allBefore.successful + inc);
            } else {
                assert(!stats[mech].hasOwnProperty("ingress"));
            }
        });
    } catch (e) {
        print("Stats: " + tojson(stats));
        throw e;
    }
    lastStats = stats;
}

const baseURI = "mongodb://admin:pwd@" + st.s.host + "/admin";

test(baseURI, fallbackMech);
test(baseURI + "?authMechanism=SCRAM-SHA-1", "SCRAM-SHA-1");
test(baseURI + "?authMechanism=SCRAM-SHA-256", "SCRAM-SHA-256");
const systemPass = cat(keyfile).replace(/\s/g, "");
test(
    "mongodb://__system:" + systemPass + "@" + st.s.host + "/admin?authMechanisms=SCRAM-SHA-256",
    "SCRAM-SHA-256",
    true,
);

admin.logout();
st.stop();

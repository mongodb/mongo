// This test just checks that the success/failure messages for authentication include the IP
// address of the client attempting to authenticate.

(function() {

const kAuthenticationSucceeded = 5286306;
const kAuthenticationFailed = 5286307;
const kIpAndPortRegex = /(?:\d{1,3}\.){3}\d{1,3}:\d+/;
const kScramShaRegex = /SCRAM-SHA-\d+/;
const conn = MongoRunner.runMongod({auth: ""});
const admin = conn.getDB("admin");

admin.createUser({
    user: "root",
    pwd: "root",
    roles: ["root"],
});

assert(admin.auth("root", "root"));

const failConn = new Mongo(conn.host);
failConn.getDB("admin").auth("root", "toot");

assert(checkLog.checkContainsWithCountJson(conn,
                                           kAuthenticationSucceeded,
                                           {user: "root", db: "admin", client: kIpAndPortRegex},
                                           1,
                                           null,
                                           true));

assert(checkLog.checkContainsWithCountJson(
    conn,
    kAuthenticationFailed,
    {user: "root", db: "admin", mechanism: kScramShaRegex, client: kIpAndPortRegex},
    1,
    null,
    true));

MongoRunner.stopMongod(conn);
})();

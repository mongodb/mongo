// These commands were removed from mongos 4.4, but will still appear in the listCommands output
// of a 4.2 mongos. A last-stable mongos will be unable to run a command on a latest version shard
// that no longer supports that command. To increase test coverage and allow us to run on same- and
// mixed-version suites, we allow these commands to have a test defined without always existing on
// the servers being used.
const commandsRemovedFromMongosIn44 = [];
// These commands were added in mongos 4.4, so will not appear in the listCommands output of a
// 4.2 mongos. We will allow these commands to have a test defined without always existing on the
// mongos being used.
const commandsAddedToMongosIn44 = ['refineCollectionShardKey'];

// These commands were removed from merizos 4.2, but will still appear in the listCommands output
// of a 4.0 merizos. A last-stable merizos will be unable to run a command on a latest version shard
// that no longer supports that command. To increase test coverage and allow us to run on same- and
// mixed-version suites, we allow these commands to have a test defined without always existing on
// the servers being used.
const commandsRemovedFromMongosIn42 = [
    'copydb',
    'copydbsaslstart',
    'eval',
    'geoNear',
    'getPrevError',
    'group',
    'reIndex',
];
// These commands were added in merizos 4.2, so will not appear in the listCommands output of a 4.0
// merizos. We will allow these commands to have a test defined without always existing on the merizos
// being used.
const commandsAddedToMongosIn42 = [
    'abortTransaction',
    'commitTransaction',
    'dropConnections',
    'setIndexCommitQuorum',
    'startRecordingTraffic',
    'stopRecordingTraffic',
];

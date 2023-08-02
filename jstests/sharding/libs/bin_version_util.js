/**
 * Utilities for checking if a node is on the latest bin version.
 */

function isLatestBinVersion(node, latestBinVersion) {
    const serverStatus = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    return MongoRunner.areBinVersionsTheSame(serverStatus.version, latestBinVersion);
}

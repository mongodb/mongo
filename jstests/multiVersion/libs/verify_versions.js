/**
 * Helpers for verifying versions of started MongoDB processes.
 */

var Mongo, assert;
(function() {
    "use strict";
    Mongo.prototype.getBinVersion = function() {
        var result = this.getDB("admin").runCommand({serverStatus: 1});
        return result.version;
    };

    // Checks that our mongodb process is of a certain version
    assert.binVersion = function(mongo, version) {
        var currVersion = mongo.getBinVersion();
        assert(MongoRunner.areBinVersionsTheSame(MongoRunner.getBinVersionFor(currVersion),
                                                 MongoRunner.getBinVersionFor(version)),
               "version " + version + " (" + MongoRunner.getBinVersionFor(version) + ")" +
                   " is not the same as " + MongoRunner.getBinVersionFor(currVersion));
    };

    // Compares an array of desired versions and an array of found versions,
    // looking for versions not found
    assert.allBinVersions = function(versionsWanted, versionsFound) {

        for (var i = 0; i < versionsWanted.length; i++) {
            var version = versionsWanted[i];
            var found = false;
            for (var j = 0; j < versionsFound.length; j++) {
                if (MongoRunner.areBinVersionsTheSame(version, versionsFound[j])) {
                    found = true;
                    break;
                }
            }

            assert(found,
                   "could not find version " + version + " (" +
                       MongoRunner.getBinVersionFor(version) + ")" + " in " + versionsFound);
        }
    };

}());

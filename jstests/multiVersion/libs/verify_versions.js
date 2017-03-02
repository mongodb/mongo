/**
 * Helpers for verifying versions of started BongoDB processes.
 */

var Bongo, assert;
(function() {
    "use strict";
    Bongo.prototype.getBinVersion = function() {
        var result = this.getDB("admin").runCommand({serverStatus: 1});
        return result.version;
    };

    // Checks that our bongodb process is of a certain version
    assert.binVersion = function(bongo, version) {
        var currVersion = bongo.getBinVersion();
        assert(BongoRunner.areBinVersionsTheSame(BongoRunner.getBinVersionFor(currVersion),
                                                 BongoRunner.getBinVersionFor(version)),
               "version " + version + " (" + BongoRunner.getBinVersionFor(version) + ")" +
                   " is not the same as " + BongoRunner.getBinVersionFor(currVersion));
    };

    // Compares an array of desired versions and an array of found versions,
    // looking for versions not found
    assert.allBinVersions = function(versionsWanted, versionsFound) {

        for (var i = 0; i < versionsWanted.length; i++) {
            var version = versionsWanted[i];
            var found = false;
            for (var j = 0; j < versionsFound.length; j++) {
                if (BongoRunner.areBinVersionsTheSame(version, versionsFound[j])) {
                    found = true;
                    break;
                }
            }

            assert(found,
                   "could not find version " + version + " (" +
                       BongoRunner.getBinVersionFor(version) + ")" + " in " + versionsFound);
        }
    };

}());

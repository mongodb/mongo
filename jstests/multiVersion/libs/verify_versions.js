/**
 * Helpers for verifying versions of started MerizoDB processes.
 */

var Merizo, assert;
(function() {
    "use strict";
    Merizo.prototype.getBinVersion = function() {
        var result = this.getDB("admin").runCommand({serverStatus: 1});
        return result.version;
    };

    // Checks that our merizodb process is of a certain version
    assert.binVersion = function(merizo, version) {
        var currVersion = merizo.getBinVersion();
        assert(MerizoRunner.areBinVersionsTheSame(MerizoRunner.getBinVersionFor(currVersion),
                                                 MerizoRunner.getBinVersionFor(version)),
               "version " + version + " (" + MerizoRunner.getBinVersionFor(version) + ")" +
                   " is not the same as " + MerizoRunner.getBinVersionFor(currVersion));
    };

    // Compares an array of desired versions and an array of found versions,
    // looking for versions not found
    assert.allBinVersions = function(versionsWanted, versionsFound) {

        for (var i = 0; i < versionsWanted.length; i++) {
            var version = versionsWanted[i];
            var found = false;
            for (var j = 0; j < versionsFound.length; j++) {
                if (MerizoRunner.areBinVersionsTheSame(version, versionsFound[j])) {
                    found = true;
                    break;
                }
            }

            assert(found,
                   "could not find version " + version + " (" +
                       MerizoRunner.getBinVersionFor(version) + ")" + " in " + versionsFound);
        }
    };

}());

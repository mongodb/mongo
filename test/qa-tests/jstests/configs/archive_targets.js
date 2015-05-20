var getDumpTarget;

(function() {
    if (getDumpTarget==null) {
     getDumpTarget = function(target) {
        if (!target) {
            return ["--archive=dump.archive"];
        }
        return ["--archive="+target];
     }
    }
})();

var getRestoreTarget;

var dump_targets = "archive";

(function() {
    if (!getRestoreTarget) {
     getRestoreTarget = function(target) {
        if (!target) {
            return ["--archive=dump.archive"];
        }
        targetParts = target.split("/")
        if ( targetParts[0]=="dump" ) {
            return ["--archive=dump.archive"];
        }
        return ["--archive="+targetParts[0]];
     }
    }
})();

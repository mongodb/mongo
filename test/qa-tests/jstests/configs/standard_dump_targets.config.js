var getDumpTarget;

(function() {
    if (getDumpTarget==null) {
     getDumpTarget = function(target) {
        if (target==null) {
            return [];
        }
        return ["--out="+target];
     }
    }
})();

var dump_targets;
if (!dump_targets) {
    dump_targets = "standard";
}

var getRestoreTarget;

(function() {
    if (getRestoreTarget==null) {
     getRestoreTarget = function(target) {
        if (target==null) {
            return [];
        }
        return ["--dir="+target];
     }
    }
})();

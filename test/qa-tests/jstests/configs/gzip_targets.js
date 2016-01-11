var getDumpTarget;

(function() {
    if (getDumpTarget==null) {
     getDumpTarget = function(target) {
        if (target==null) {
            return ["--gzip"];
        }
        if (target.indexOf(".bson", target.length - 5) != -1) {
            return ["--gzip", "--out="+target+".gz"];
        }
        return ["--gzip", "--out="+target];
     }
    }
})();

var dump_targets;
if (!dump_targets) {
    dump_targets = "gzip";
}

var getRestoreTarget;

(function() {
    if (getRestoreTarget==null) {
     getRestoreTarget = function(target) {
        if (target==null) {
            return ["--gzip"];
        }
        if (target.indexOf(".bson", target.length - 5) != -1) {
            return ["--gzip", "--dir="+target+".gz"];
        }
        return ["--gzip", "--dir="+target];
     }
    }
})();

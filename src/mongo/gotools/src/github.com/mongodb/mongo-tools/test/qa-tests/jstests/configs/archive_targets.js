var getDumpTarget;

(function() {
  if (getDumpTarget === undefined) {
    getDumpTarget = function(target) {
      if (!target) {
        return ["--archive=dump.archive"];
      }
      return ["--archive="+target];
    };
  }
}());

var getRestoreTarget;

/* exported dump_targets */
var dump_targets = "archive";

(function() {
  if (getRestoreTarget === undefined) {
    getRestoreTarget = function(target) {
      if (!target) {
        return ["--archive=dump.archive"];
      }
      targetParts = target.split("/");
      if (targetParts[0] === "dump") {
        return ["--archive=dump.archive"];
      }
      return ["--archive="+targetParts[0]];
    };
  }
}());

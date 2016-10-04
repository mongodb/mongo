var _orig_runMongoProgram = runMongoProgram;
runMongoProgram = function() {
  var args = [];
  for (var i in arguments) {
    args[i] = arguments[i];
  }
  var progName = args[0];
  if (progName !== "bsondump" && args.indexOf("--dialTimeout") === -1) {
    args.push("--dialTimeout", "30");
  }
  return _orig_runMongoProgram.apply(null, args);
};

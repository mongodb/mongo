var getVersionComponents = function(version) {
  var splitVersion = version.split('.');
  assert.eq(3, splitVersion.length);
  var major = parseInt(splitVersion[0], 10);
  var minor = parseInt(splitVersion[1], 10);

  var patchEnd = splitVersion[2].indexOf('-') !== -1 ?
    splitVersion[2].indexOf('-') :
    undefined;
  var patch = parseInt(splitVersion[2].substr(0, patchEnd));
  return {
    major: major,
    minor: minor,
    patch: patch
  };
};

var isAtLeastVersion = function(serverVersion, checkVersion) {
  serverVersion = getVersionComponents(serverVersion);
  checkVersion = getVersionComponents(checkVersion);

  return (checkVersion.major < serverVersion.major) ||
    (checkVersion.major === serverVersion.major &&
      checkVersion.minor < serverVersion.minor) ||
    (checkVersion.major === serverVersion.major &&
      checkVersion.minor === serverVersion.minor &&
      checkVersion.patch <= serverVersion.patch);
};

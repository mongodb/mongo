/**
 *  Given a MongoDB version, parses it into its major/minor/patch components,
 *  discounting '-pre' and '-rcX'. Useful for parsing the output of
 *  `db.version()` into an appropriate form for comparisons.
 *
 *  Examples:
 *    getVersionComponents('2.7.8'); // { major: 2, minor: 7, patch: 8 }
 *    getVersionComponents('2.8.0-rc0'); // { major: 2, minor: 8, patch: 0 }
 */
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
    patch: patch,
  };
};

/**
 *  Given two versions, returns true if the first version is >= the second.
 *
 *  Examples:
 *    isAtLeastVersion('2.7.8', '2.7.8'); // true
 *    isAtLeastVersion('2.8.0-rc0', '2.7.8'); // true
 *    isAtLeastVersion('2.6.6', '2.7.8'); // false
 *    isAtLeastVersion('1.8.5', '2.7.8'); // false
 */
/* exported isAtLeastVersion */
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

// type declarations for shell_utils.h

declare function _buildBsonObj()
declare function _closeGoldenData()
declare function _compareStringsWithCollation()
declare function _createSecurityToken()
declare function _createTenantToken()
declare function _fnvHashToHexString()
declare function _isWindows()
declare function _openGoldenData()
declare function _rand()
declare function _replMonitorStats()
/**
 * Compares two result sets after applying some normalizations. This function should only
 * be used in the fuzzer.
 * 
 * @param a First result set.
 * @param b Second result set.
 * 
 * @throws {Error} If the size of the BSON representation of 'a' and 'b' exceeds the BSON size limit
 *                 (~16mb).
 * 
 * @returns True if the result sets compare equal and false otherwise.
 */
declare function _resultSetsEqualNormalized(a: object[], b: object[]): boolean
declare function _resultSetsEqualUnordered()
declare function _setShellFailPoint()
declare function _srand()
declare function _writeGoldenData()
declare function benchRun()
declare function benchRunSync()
declare function computeSHA256Block()
declare function convertShardKeyToHashed()
declare function fileExists()
declare function getBuildInfo()
declare function getMemInfo()
declare function interpreterVersion()
declare function isInteractive()
declare function numberDecimalsAlmostEqual()
declare function numberDecimalsEqual()

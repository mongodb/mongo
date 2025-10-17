// type declarations for shell_utils_extended.h

declare function _copyFileRange()
declare function _getEnv()

/**
 * Retrieves the nth BSONObj in a BSON dump file
 * 
 * @param filename {String} filename of the dump file
 * @param n {Number} index of the object to retrieve
 * 
 * @returns {BSONObj} fetched object
 */
declare function _getObjInDumpFile()

/**
 * Retrieves the number of BSONObj in a BSON dump file
 * 
 * @param filename {String} filename of the dump file
 * 
 * @returns {Number} number of objects
 */
declare function _numObjsInDumpFile()

declare function _readDumpFile()
declare function appendFile()
declare function cat()
declare function cd()
declare function copyDir()
declare function copyFile()
declare function decompressBSONColumn()
declare function getFileMode()
declare function getHostName()
declare function getStringWidth()
declare function hostname()
declare function listFiles()
declare function ls()
declare function md5sumFile()
declare function mkdir()
declare function passwordPrompt()
declare function pwd()
declare function removeFile()
declare function umask()
declare function writeFile()

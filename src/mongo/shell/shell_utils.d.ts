// type declarations for shell_utils.h

/**
 * Build a BSON object from JavaScript object.
 * Internal utility for converting JavaScript objects to BSON format.
 * @param obj JavaScript object to convert
 * @returns BSON object representation
 */
declare function _buildBsonObj(obj: object): BSONObj

/**
 * Close a golden data file for testing.
 * Used internally by the test framework to clean up golden data file handles.
 * @param handle File handle returned by _openGoldenData()
 */
declare function _closeGoldenData(handle: any): void

/**
 * Compare two strings using the specified collation rules.
 * Performs locale-aware string comparison according to MongoDB collation specification.
 * @param str1 First string to compare
 * @param str2 Second string to compare
 * @param collation Collation specification object (locale, strength, caseLevel, etc.)
 * @returns Negative if str1 < str2, positive if str1 > str2, zero if equal
 */
declare function _compareStringsWithCollation(str1: string, str2: string, collation: object): number

/**
 * Create a security token for testing.
 * Used for testing authentication and authorization with security tokens.
 * @param user User object specifying user credentials and properties
 * @returns Security token object
 */
declare function _createSecurityToken(user: object): object

/**
 * Create a tenant token for multi-tenancy testing.
 * Used for testing multi-tenant deployments with tenant isolation.
 * @param tenant Tenant object specifying tenant properties
 * @returns Tenant token object
 */
declare function _createTenantToken(tenant: object): object

/**
 * Get Decimal128 maximum and minimum positive and negative values.
 * Returns the largest or smallest representable Decimal128 value.
 * @param str String matching [+-]?(max|min), case insensitive (e.g., "max", "-min", "+max")
 * @returns NumberDecimal representing the requested limit value
 */
declare function _decimal128Limit(str: string): NumberDecimal

/**
 * Hash a string using FNV-1a algorithm and return as hex string.
 * Used for generating consistent hash values for testing purposes.
 * @param str String to hash
 * @returns Hexadecimal string representation of the hash value
 */
declare function _fnvHashToHexString(str: string): string

/**
 * Check if the shell is running on Windows.
 * @returns True if running on Windows, false otherwise
 */
declare function _isWindows(): boolean

/**
 * Open a golden data file for testing and return handle.
 * Used by the test framework to compare actual results against expected golden data.
 * @param filename Path to the golden data file
 * @returns File handle for use with _writeGoldenData() and _closeGoldenData()
 */
declare function _openGoldenData(filename: string): any

/**
 * Generate a random number between 0 and 1.
 * Uses the shell's internal random number generator (seeded with _srand).
 * @returns Random number in the range [0, 1)
 */
declare function _rand(): number

/**
 * Get replica set monitor statistics for a host.
 * Returns diagnostic information about replica set monitoring and server selection.
 * @param host Optional host string to filter statistics for (e.g., "localhost:27017")
 * @returns Object containing replica set monitor statistics
 */
declare function _replMonitorStats(host?: string): object
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
/**
 * Compares two result sets after sorting arrays.
 *
 * @param a First result set.
 * @param b Second result set.
 *
 * @throws {Error} If the size of the BSON representation of 'a' and 'b' exceeds the BSON size limit
 *                 (~16mb).
 *
 * @returns True if the result sets compare equal after sorting arrays and false otherwise.
 */
declare function _resultSetsEqualUnorderedWithUnorderedArrays(a: object[], b: object[]): boolean

/**
 * Compare two result sets ignoring order.
 * Tests if two arrays of documents contain the same elements, regardless of order.
 * @param a First result set (array of documents)
 * @param b Second result set (array of documents)
 * @returns True if the result sets contain the same documents (order-independent), false otherwise
 */
declare function _resultSetsEqualUnordered(a: object[], b: object[]): boolean

/**
 * Set a shell-side fail point for testing.
 * Used to inject failures in the shell for testing error handling paths.
 * @param options Fail point configuration object with mode, data, and other settings
 * @returns Result object indicating success or failure
 */
declare function _setShellFailPoint(options: object): object

/**
 * Seed the random number generator.
 * Seeds the shell's internal random number generator for reproducible test results.
 * @param seed Integer seed value
 */
declare function _srand(seed: number): void

/**
 * Write data to a golden data file for testing.
 * Used by the test framework to record expected results for comparison.
 * @param data Data to write to the golden file
 * @param filename Path to the golden data file
 */
declare function _writeGoldenData(data: any, filename: string): void

/**
 * Run a benchmark workload and return statistics.
 * Executes a parallel workload for performance testing and benchmarking.
 * @param config Benchmark configuration object specifying operations, thread count, duration, etc.
 * @returns Object containing performance statistics (ops/sec, latency, etc.)
 */
declare function benchRun(config: object): object

/**
 * Run a benchmark workload synchronously.
 * Executes a parallel workload and waits for completion before returning statistics.
 * @param config Benchmark configuration object specifying operations, thread count, duration, etc.
 * @returns Object containing performance statistics (ops/sec, latency, etc.)
 */
declare function benchRunSync(config: object): object

/**
 * Compute SHA256 hash of a message.
 * Generates a SHA-256 cryptographic hash of the input message.
 * @param message String or BinData to hash
 * @returns BinData containing the SHA-256 hash (32 bytes)
 */
declare function computeSHA256Block(message: string | BinData): BinData

/**
 * Compute HMAC-SHA256 of a message with a key.
 * Generates a keyed-hash message authentication code using SHA-256.
 * @param key Secret key as string or BinData
 * @param message Message to authenticate as string or BinData
 * @returns BinData containing the HMAC-SHA256 result (32 bytes)
 */
declare function computeSHA256Hmac(key: string | BinData, message: string | BinData): BinData

/**
 * Convert a shard key to its hashed value.
 * Computes the hash value used by MongoDB's hashed sharding strategy.
 * @param shardKey Shard key value to hash
 * @returns NumberLong representing the hashed shard key value
 */
declare function convertShardKeyToHashed(shardKey: any): NumberLong

/**
 * Check if a file exists at the given path.
 * @param path File path to check
 * @returns True if the file exists, false otherwise
 */
declare function fileExists(path: string): boolean

/**
 * Get MongoDB build information.
 * Returns information about the MongoDB server build, including version, modules, and features.
 * @returns Object containing version, gitVersion, modules, allocator, and other build details
 */
declare function getBuildInfo(): object

/**
 * Get system memory information.
 * Returns details about system memory usage and availability.
 * @returns Object containing memory statistics (total, available, etc.)
 */
declare function getMemInfo(): object

/**
 * Get the JavaScript interpreter version.
 * Returns the version string of the JavaScript engine used by the shell.
 * @returns Version string (e.g., "MozJS-115")
 */
declare function interpreterVersion(): string

/**
 * Check if the shell is running in interactive mode.
 * Interactive mode means the shell is connected to a terminal and accepting user input.
 * @returns True if running in interactive mode, false if running a script non-interactively
 */
declare function isInteractive(): boolean

/**
 * Check if two NumberDecimal values are almost equal within specified decimal places.
 * Uses approximate comparison to account for rounding differences.
 * @param a First NumberDecimal value
 * @param b Second NumberDecimal value
 * @param places Number of decimal places to compare (default: 15)
 * @returns True if the values are equal within the specified precision
 */
declare function numberDecimalsAlmostEqual(a: NumberDecimal, b: NumberDecimal, places?: number): boolean

/**
 * Check if two NumberDecimal values are exactly equal.
 * Performs exact comparison of Decimal128 values.
 * @param a First NumberDecimal value
 * @param b Second NumberDecimal value
 * @returns True if the values are exactly equal, false otherwise
 */
declare function numberDecimalsEqual(a: NumberDecimal, b: NumberDecimal): boolean

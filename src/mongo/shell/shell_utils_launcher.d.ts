// type declarations for shell_utils_launcher.h

/**
 * Read all available test pipe names.
 * Returns the names of all test pipes that have been created for inter-process communication.
 * @returns Array of test pipe name strings
 */
declare function _readTestPipes(): string[]

/**
 * Run a MongoDB program with arguments and wait for completion.
 * Starts a MongoDB program (mongod, mongos, etc.) and blocks until it exits.
 * @param args Program name followed by command-line arguments
 * @returns Exit code of the program
 */
declare function _runMongoProgram(...args: string[]): number

/**
 * Get PIDs of all running child MongoDB processes.
 * Returns process IDs of all MongoDB programs started by the shell.
 * @returns Array of process IDs (PIDs)
 */
declare function _runningMongoChildProcessIds(): number[]

/**
 * Start a MongoDB program with arguments.
 * Starts a MongoDB program (mongod, mongos, etc.) in the background and returns immediately.
 * @param args Program name followed by command-line arguments
 * @returns Process ID (PID) of the started program
 */
declare function _startMongoProgram(...args: string[]): number

/**
 * Stop a MongoDB program running on the specified port.
 * Sends a signal to terminate the MongoDB process listening on the given port.
 * @param port Port number the MongoDB process is listening on
 * @param signal Optional signal number (default: SIGTERM/15)
 */
declare function _stopMongoProgram(port: number, signal?: number): void

/**
 * Write a value to a named test pipe.
 * Used for inter-process communication in tests.
 * @param pipe Name of the test pipe
 * @param value Value to write (will be converted to JSON)
 */
declare function _writeTestPipe(pipe: string, value: any): void

/**
 * Write a BSON object to a test pipe file.
 * Writes a single BSON document to a test pipe for consumption by another process.
 * @param pipe Name of the test pipe
 * @param obj Object to serialize as BSON
 */
declare function _writeTestPipeBsonFile(pipe: string, obj: object): void

/**
 * Write a BSON object to a test pipe file synchronously.
 * Writes a single BSON document to a test pipe and ensures it's flushed before returning.
 * @param pipe Name of the test pipe
 * @param obj Object to serialize as BSON
 */
declare function _writeTestPipeBsonFileSync(pipe: string, obj: object): void

/**
 * Write multiple BSON objects to a test pipe.
 * Writes an array of BSON documents to a test pipe for batch processing.
 * @param pipe Name of the test pipe
 * @param objs Array of objects to serialize as BSON documents
 */
declare function _writeTestPipeObjects(pipe: string, objs: object[]): void

/**
 * Check if a program is running on the specified port.
 * Tests if a MongoDB process is still running and responsive on the given port.
 * @param port Port number to check
 * @returns Object with status information about the program
 */
declare function checkProgram(port: number): object

/**
 * Clear the captured output from MongoDB programs.
 * Clears the output buffer for programs started with _startMongoProgram.
 * @param port Optional port number to clear output for specific program (omit for all)
 */
declare function clearRawMongoProgramOutput(port?: number): void

/**
 * Convert a traffic recording file to BSON format.
 * Converts a captured network traffic recording into BSON documents.
 * @param inputFile Path to the input traffic recording file
 * @param outputFile Path to the output BSON file
 * @param args Additional arguments for conversion options
 * @returns Result object with conversion statistics
 */
declare function convertTrafficRecordingToBSON(inputFile: string, outputFile: string, ...args: string[]): object

/**
 * Copy a database path directory.
 * Copies all files from one dbpath directory to another for testing purposes.
 * @param from Source dbpath directory
 * @param to Destination dbpath directory
 */
declare function copyDbpath(from: string, to: string): void

/**
 * Get Feature Compatibility Version (FCV) constants.
 * Returns an object containing FCV version strings for use in version testing.
 * @returns Object with FCV constants (e.g., latestFCV, lastContinuousFCV, lastLTSFCV)
 */
declare function getFCVConstants(): object

/**
 * Check if a path exists.
 * Tests whether a file or directory exists at the given path.
 * @param path File or directory path to check
 * @returns True if the path exists, false otherwise
 */
declare function pathExists(path: string): boolean

/**
 * Get the raw output from a MongoDB program by port.
 * Retrieves all captured stdout/stderr from a program started with _startMongoProgram.
 * @param port Optional port number to get output for specific program (omit for all)
 * @returns Raw output string from the program(s)
 */
declare function rawMongoProgramOutput(port?: number): string

/**
 * Reset (clear) a database path directory.
 * Deletes all files in a dbpath directory to start with a clean state.
 * @param path Path to the dbpath directory to clear
 */
declare function resetDbpath(path: string): void

/**
 * Run a program with arguments and wait for completion.
 * General-purpose function to run any program and wait for it to exit.
 * @param program Program name or path
 * @param args Command-line arguments
 * @returns Exit code of the program
 */
declare function run(program: string, ...args: string[]): number

/**
 * Run a non-MongoDB program with arguments.
 * Runs any non-MongoDB program and waits for completion, capturing output.
 * @param program Program name or path
 * @param args Command-line arguments
 * @returns Exit code of the program
 */
declare function runNonMongoProgram(program: string, ...args: string[]): number

/**
 * Run a non-MongoDB program quietly without output.
 * Runs any non-MongoDB program without capturing or displaying output.
 * @param program Program name or path
 * @param args Command-line arguments
 * @returns Exit code of the program
 */
declare function runNonMongoProgramQuietly(program: string, ...args: string[]): number

/**
 * Run a program with arguments.
 * Alias for run() - runs any program and waits for completion.
 * @param program Program name or path
 * @param args Command-line arguments
 * @returns Exit code of the program
 */
declare function runProgram(program: string, ...args: string[]): number

/**
 * Stop a program by PID.
 * Sends a signal to terminate a program identified by its process ID.
 * @param pid Process ID to stop
 * @param signal Optional signal number (default: SIGTERM/15)
 */
declare function stopMongoProgramByPid(pid: number, signal?: number): void

/**
 * Wait for a MongoDB program on the specified port to exit.
 * Blocks until the MongoDB program listening on the given port terminates.
 * @param port Port number the MongoDB process is listening on
 * @returns Exit code of the program
 */
declare function waitMongoProgram(port: number): number

/**
 * Wait for a program with the specified PID to exit.
 * Blocks until the program with the given process ID terminates.
 * @param pid Process ID to wait for
 * @returns Exit code of the program
 */
declare function waitProgram(pid: number): number

/**
 * Replay a workload recording file against a MongoDB instance.
 * Replays a previously recorded workload for performance testing or reproduction.
 * @param filename Path to the workload recording file
 * @param connectionString Optional MongoDB connection string (default: localhost:27017)
 */
declare function replayWorkloadRecordingFile(filename: string, connectionString?: string): void

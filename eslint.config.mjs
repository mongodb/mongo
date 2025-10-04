import {FlatCompat} from "@eslint/eslintrc";
import eslint from "@eslint/js";
import js from "@eslint/js";
import {default as mongodb_plugin} from "eslint-plugin-mongodb";
import globals from "globals";
import path from "node:path";
import {fileURLToPath} from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const compat = new FlatCompat({
    baseDirectory: __dirname,
    recommendedConfig: js.configs.recommended,
    allConfig: js.configs.all,
});

export default [
    ...compat.extends("eslint:recommended"),
    {
        ignores: ["src/mongo/gotools/*", "**/*.tpl.js", "jstests/third_party/**/*.js"],
    },
    {
        languageOptions: {
            globals: {
                ...globals.mongo,

                // jstests/global.d.ts
                TestData: true,

                // jstests/libs/parallelTester.d.ts
                CountDownLatch: true,

                // src/mongo/shell/assert_global.js
                assert: true,
                doassert: true,
                sortDoc: true,
                formatErrorMsg: true,

                // src/mongo/shell/bridge_global.js
                MongoBridge: true,

                // src/mongo/shell/bulk_api_global.js
                BulkWriteError: true,
                WriteCommandError: true,
                WriteConcern: true,
                WriteError: true,

                // src/mongo/shell/check_log_global.js
                checkLog: true,

                // src/mongo/shell/collection.js
                DBCollection: true,

                // src/mongo/shell/data_consistency_checker_global.js
                CollInfos: true,
                DataConsistencyChecker: true,

                // src/mongo/shell/db.d.ts
                DB: true,

                // src/mongo/shell/error_codes.d.ts
                ErrorCodeStrings: true,
                ErrorCodes: true,

                // src/mongo/shell/explain_query_global.js
                DBExplainQuery: true,

                // src/mongo/shell/explainable_global.js
                Explainable: true,

                // src/mongo/shell/feature_compatibility_version_global.js
                binVersionToFCV: true,
                checkFCV: true,
                isFCVEqual: true,
                lastContinuousFCV: true,
                lastLTSFCV: true,
                latestFCV: true,
                numVersionsSinceLastLTS: true,
                removeFCVDocument: true,
                runFeatureFlagMultiversionTest: true,

                // src/mongo/shell/query_global.js
                DBQuery: true,
                DBCommandCursor: true,
                QueryHelpers: true,
                ___it___: true,

                // src/mongo/shell/servers_global.js
                MongoRunner: true,
                myPort: true,
                runMongoProgram: true,
                startMongoProgram: true,
                startMongoProgramNoConnect: true,

                // src/mongo/shell/servers_misc_global.js
                ToolTest: true,
                allocatePort: true,
                allocatePorts: true,
                resetAllocatedPorts: true,
                startParallelShell: true,
                testingReplication: true,
                uncheckedParallelShellPidsString: true,

                // src/mongo/shell/session_global.js
                DriverSession: true,
                SessionOptions: true,
                _DelegatingDriverSession: true,
                _DummyDriverSession: true,
                _ServerSession: true,

                // src/mongo/shell/shell_utils.d.ts
                _buildBsonObj: true,
                _closeGoldenData: true,
                _compareStringsWithCollation: true,
                _createSecurityToken: true,
                _createTenantToken: true,
                _fnvHashToHexString: true,
                _isWindows: true,
                _openGoldenData: true,
                _rand: true,
                _replMonitorStats: true,
                _resultSetsEqualNormalized: true,
                _resultSetsEqualUnordered: true,
                _setShellFailPoint: true,
                _srand: true,
                _writeGoldenData: true,
                benchRun: true,
                benchRunSync: true,
                computeSHA256Block: true,
                convertShardKeyToHashed: true,
                fileExists: true,
                getBuildInfo: true,
                interpreterVersion: true,
                isInteractive: true,
                numberDecimalsAlmostEqual: true,
                numberDecimalsEqual: true,
                replayWorkloadRecordingFile: true,

                // src/mongo/shell/shell_utils_extended.d.ts
                _copyFileRange: true,
                _getEnv: true,
                _readDumpFile: true,
                appendFile: true,
                copyDir: true,
                copyFile: true,
                decompressBSONColumn: true,
                dumpBSONAsHex: true,
                getFileMode: true,
                getStringWidth: true,
                hexToBSON: true,
                passwordPrompt: true,
                umask: true,
                writeFile: true,

                // src/mongo/shell/shell_utils_launcher.d.ts
                _readTestPipes: true,
                _runMongoProgram: true,
                _runningMongoChildProcessIds: true,
                _startMongoProgram: true,
                _stopMongoProgram: true,
                _writeTestPipe: true,
                _writeTestPipeBsonFile: true,
                _writeTestPipeBsonFileSync: true,
                _writeTestPipeObjects: true,
                checkProgram: true,
                clearRawMongoProgramOutput: true,
                convertTrafficRecordingToBSON: true,
                copyDbpath: true,
                getFCVConstants: true,
                pathExists: true,
                rawMongoProgramOutput: true,
                resetDbpath: true,
                run: true,
                runNonMongoProgram: true,
                runNonMongoProgramQuietly: true,
                runProgram: true,
                stopMongoProgramByPid: true,
                waitMongoProgram: true,
                waitProgram: true,

                // src/mongo/shell/types.d.ts
                BSONAwareMap: true,
                isNumber: true,
                isObject: true,
                isString: true,
                printjson: true,
                printjsononeline: true,
                toJsonForLog: true,
                tojson: true,
                tojsonObject: true,
                tojsononeline: true,

                // src/mongo/shell/utils_global.js
                Geo: true,
                Random: true,
                __autocomplete__: true,
                __magicNoPrint: true,
                __promptWrapper__: true,
                __prompt__: true,
                __quiet: true,
                _awaitRSHostViaRSMonitor: true,
                _getErrorWithCode: true,
                _isSpiderMonkeyDebugEnabled: true,
                _shouldRetryWrites: true,
                _shouldUseImplicitSessions: true,
                _verboseShell: true,
                chatty: true,
                compare: true,
                compareOn: true,
                defaultPrompt: true,
                friendlyEqual: true,
                hasErrorCode: true,
                help: true,
                indentStr: true,
                isNetworkError: true,
                isRetryableError: true,
                jsTest: true,
                jsTestOptions: true,
                jsTestLog: true,
                jsTestName: true,
                printStackTrace: true,
                retryOnNetworkError: true,
                retryOnRetryableError: true,
                shellAutocomplete: true,
                shellHelper: true,
                shellPrintHelper: true,
                setVerboseShell: true,
                timestampCmp: true,

                // src/mongo/shell/utils_auth_global.js
                authutil: true,

                // src/mongo/shell/utils_sh_global.js
                printShardingStatus: true,

                // src/mongo/scripting/mozjs/bindata.d.ts
                BinData: true,
                HexData: true,
                MD5: true,

                // src/mongo/scripting/mozjs/bson.d.ts
                bsonBinaryEqual: true,
                bsonObjToArray: true,
                bsonUnorderedFieldsCompare: true,
                bsonWoCompare: true,

                // src/mongo/scripting/mozjs/code.d.ts
                Code: true,

                // src/mongo/scripting/mozjs/dbpointer.d.ts
                DBPointer: true,

                // src/mongo/scripting/mozjs/dbref.d.ts
                DBRef: true,

                // src/mongo/scripting/mozjs/global.d.ts
                buildInfo: true,
                gc: true,
                getJSHeapLimitMB: true,
                print: true,
                sleep: true,

                // src/mongo/scripting/mozjs/jsthread.d.ts
                _threadInject: true,

                // src/mongo/scripting/mozjs/maxkey.d.ts
                MaxKey: true,

                // src/mongo/scripting/mozjs/minkey.d.ts
                MinKey: true,

                // src/mongo/scripting/mozjs/mongo.d.ts
                _forgetReplSet: true,

                // src/mongo/scripting/mozjs/numberdecimal.d.ts
                NumberDecimal: true,

                // src/mongo/scripting/mozjs/numberlong.d.ts
                NumberLong: true,

                // src/mongo/scripting/mozjs/object.d.ts
                bsonsize: true,

                // src/mongo/scripting/mozjs/resumetoken.d.ts
                decodeResumeToken: true,
                eventResumeTokenType: true,
                highWaterMarkResumeTokenType: true,

                // src/mongo/scripting/mozjs/timestamp.d.ts
                Timestamp: true,

                // src/mongo/scripting/mozjs/uri.d.ts
                MongoURI: true,

                // src/mongo/scripting/utils.d.ts
                hex_md5: true,
                tostrictjson: true,

                // TODO: where are these defined?
                debug: true,
                emit: true,
                port: true,
            },

            ecmaVersion: 2022,
            sourceType: "module",
        },

        plugins: {
            mongodb: mongodb_plugin,
        },

        rules: {
            // TODO SERVER-99571 : enable mongodb/* rules.
            "mongodb/no-print-fn": 0,
            "mongodb/no-printing-tojson": 0,

            "no-prototype-builtins": 0,
            "no-useless-escape": 0,
            "no-irregular-whitespace": 0,
            "no-inner-declarations": 0,

            "no-unused-vars": [
                0,
                {
                    varsIgnorePattern: "^_",
                    args: "none",
                },
            ],

            "no-empty": 0,
            "no-redeclare": 0,
            "no-constant-condition": 0,
            "no-loss-of-precision": 0,
            "no-unexpected-multiline": 0,
            semi: 2,

            "no-restricted-syntax": [
                "error",
                {
                    message: "Invalid load call. Please convert your library to a module and import it instead.",
                    selector: 'CallExpression > Identifier[name="load"]',
                },
            ],
        },
    },
    {
        // It's ok for golden tests to use print() and tojson() directly.
        plugins: {
            mongodb: mongodb_plugin,
        },
        files: [
            "jstests/libs/begin_golden_test.js",
            "jstests/libs/golden_test.js",
            "jstests/libs/override_methods/golden_overrides.js",
            "jstests/libs/override_methods/sharded_golden_overrides.js",
            "jstests/libs/query/golden_test_utils.js",
            "jstests/libs/query_golden_sharding_utils.js",
            "jstests/query_golden/**/*.js",
            "jstests/query_golden_sharding/**/*.js",
        ],
        rules: {
            "mongodb/no-print-fn": 0,
            "mongodb/no-tojson-fn": 0,
        },
    },
    {
        // Don't run mongodb linter rules on src/
        plugins: {
            mongodb: mongodb_plugin,
        },
        files: ["src/**/*.js"],
        rules: {
            "mongodb/no-print-fn": 0,
            "mongodb/no-tojson-fn": 0,
        },
    },
    {
        // Shell-specific: extra strict!
        files: ["jstests/core/js/**", "jstests/noPassthrough/shell/**", "src/mongo/shell/**"],
        rules: {
            "logical-assignment-operators": 2,
            "no-extra-bind": 2,
            "no-irregular-whitespace": 2,
            "no-loss-of-precision": 2,
            "no-unneeded-ternary": 2,
            "no-unused-vars": [2, {args: "none", caughtErrors: "none"}],
            "no-useless-call": 2,
            "no-useless-escape": 2,
            "no-var": 2,
            "object-shorthand": 2,
            "prefer-object-has-own": 2,
            "prefer-object-spread": 2,
            "prefer-rest-params": 2,
            "prefer-spread": 2,
        },
    },
];

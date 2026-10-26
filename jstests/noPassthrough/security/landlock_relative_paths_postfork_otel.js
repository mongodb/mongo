/**
 * The OpenTelemetry part of landlock_relative_paths_postfork.js: verifies the Landlock grants for
 * the OTel exporters' output paths when they are configured relative on a --fork'd server.
 *
 * @tags: [
 *   requires_otel_build,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    pathRelativeToDaemonCwd,
    pathRelativeToStartupCwd,
    getLandlockRootRelativeScratchDirectory,
    startForkedMongod,
    stopForkedMongod,
    assertLandlockMode,
} from "jstests/noPassthrough/security/landlock_helpers.js";
import {
    enableFullSampling,
    kNoSamplingStrategy,
    samplingConfigForStartup,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

if (getBuildInfo().buildEnvironment.target_os !== "linux") {
    jsTest.log.info("Landlock is Linux-only; skipping test");
    quit();
}

// The shell's cwd which every "pre-fork" relative option is resolved against.
const kCwd = pwd();

// Scratch hierarchy for the paths resolved against the launcher's cwd.
const kRoot = MongoRunner.dataPath + jsTestName();
resetDbpath(kRoot);

function assertLandlockStatus(conn) {
    return assertLandlockMode(conn, "bestEffort");
}

/**
 * Runs one exporter's case: starts a forked daemon whose OTel output paths are named relative to
 * "/", asserts the sandbox engaged, and waits for the exporter to write where those paths resolve.
 * `outputPaths(root)` returns the case's absolute paths, of which `dirs` are created before the
 * launch and `expectOutputIn` are waited on afterwards; `makeParams` turns them into the node's
 * setParameters. `afterStart`, if given, runs against the live node before any traffic, for an
 * exporter that has to be armed at runtime before it will emit anything.
 */
function describeExporter(desc, name, makeParams, outputPaths, afterStart) {
    describe(desc, function () {
        const forkRoot = `${kRoot}/${name}`;
        const dbDir = `${forkRoot}/db`;
        const logFile = `${forkRoot}/mongod.log`;
        // Under /var/tmp rather than /tmp, which the policy grants statically and which would
        // therefore mask a rule derived against the wrong base.

        const rootRelRoot = getLandlockRootRelativeScratchDirectory(name);
        const paths = outputPaths(rootRelRoot);
        const port = allocatePort();

        let conn;
        let daemonPid;

        before(function () {
            mkdir(dbDir);
            for (const dir of paths.dirs) {
                mkdir(dir);
            }

            ({conn, pid: daemonPid} = startForkedMongod({
                port: port,
                fork: "",
                landlockMode: "bestEffort",
                dbpath: pathRelativeToStartupCwd(dbDir),
                logpath: pathRelativeToStartupCwd(logFile),
                pidfilepath: `${forkRoot}/mongod.pid`,
                setParameter: makeParams(paths),
            }));
        });

        it("exports where the root-relative paths resolve, under the sandbox", function () {
            assertLandlockStatus(conn);

            if (afterStart) {
                afterStart(conn);
            }

            // Generate some traffic, so an exporter that only writes when there is something to
            // report has something to report.
            const coll = conn.getDB(jsTestName()).coll;
            assert.commandWorked(coll.insert({_id: 0}));
            assert.eq(1, coll.countDocuments({}));

            // The exporters write on their own schedule, so wait rather than sample once.
            for (const dir of paths.expectOutputIn) {
                assert.soon(
                    () => listFiles(dir).length > 0,
                    `nothing was exported into ${dir}`,
                    60 * 1000,
                );
            }
        });

        after(function () {
            stopForkedMongod(conn, daemonPid);
            // Outside the test's data directory, so resmoke will not clean it up.
            removeFile(rootRelRoot);
        });
    });
}

describeExporter(
    "relative OTel metrics and trace directories with --fork",
    "files",
    ({metricsDir, tracesDir}) => ({
        openTelemetryMetricsDirectory: pathRelativeToDaemonCwd(metricsDir),
        opentelemetryTraceDirectory: pathRelativeToDaemonCwd(tracesDir),
        featureFlagOtelTraceSampling: true,
        openTelemetryTracingSampling: samplingConfigForStartup({
            defaultSampling: kNoSamplingStrategy,
        }),
        openTelemetryTracingBatchExportIntervalMillis: 500,
    }),
    (root) => {
        const metricsDir = `${root}/metrics`;
        const tracesDir = `${root}/traces`;
        return {
            metricsDir,
            tracesDir,
            dirs: [metricsDir, tracesDir],
            expectOutputIn: [metricsDir, tracesDir],
        };
    },
    (conn) => enableFullSampling(conn, {spanNames: ["insert"]}),
);

// The two Prometheus parameters get a node each. They are one exporter as far as
// validateOptions() is concerned and may legally be set together, but the path takes precedence
// when both are given -- so a node with both would leave the directory's grant derived and never
// exercised, which is the case the sandbox needs proven.
describeExporter(
    "relative OTel Prometheus metrics directory with --fork",
    "prometheus-dir",
    ({prometheusDir}) => ({
        openTelemetryPrometheusMetricsDirectory: pathRelativeToDaemonCwd(prometheusDir),
    }),
    (root) => {
        // The exporter names the file itself, "mongodb-prometheus-metrics.txt", so the grant is
        // on the directory (metrics_initialization.cpp:266).
        const prometheusDir = `${root}/prometheus`;
        return {
            prometheusDir,
            dirs: [prometheusDir],
            expectOutputIn: [prometheusDir],
        };
    },
);

describeExporter(
    "relative OTel Prometheus metrics path with --fork",
    "prometheus-path",
    ({prometheusFile}) => ({
        openTelemetryPrometheusMetricsPath: pathRelativeToDaemonCwd(prometheusFile),
    }),
    (root) => {
        // This one names the file, so the policy grants its parent: a ".tmp" sibling is written
        // and renamed over the target rather than the file being rewritten in place.
        const pathDir = `${root}/prometheus-path`;
        return {
            prometheusFile: `${pathDir}/metrics.txt`,
            dirs: [pathDir],
            expectOutputIn: [pathDir],
        };
    },
);

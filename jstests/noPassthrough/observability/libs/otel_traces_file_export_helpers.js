import {
    findOtelFilesWithSuffix,
    readJsonlFile,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

/**
 * Generates the name for a tracing directory for the given test name and ensures the directory
 * exists. The name is randomized so the same testName will provide different results on different
 * calls.
 * @param {string} testName - Used as a prefix for the directory name.
 * @returns {string} The absolute path to the newly created directory.
 */
export function createTraceDirectory(testName) {
    if (!Random.isInitialized()) {
        Random.setRandomSeed();
    }
    const traceDir = MongoRunner.toRealPath(`${testName}_otel_traces_${Random.randInt(1000000)}`);
    assert(mkdir(traceDir), `Failed to create trace directory: ${traceDir}`);
    return traceDir;
}

/**
 * Finds trace files in the given directory.
 * @param {string} directory - The directory path to search in.
 * @returns {Array<Object>} An array of file objects (from listFiles()) whose names end with
 *     "-trace.jsonl" (the suffix for the opentelemetry trace files in JSONL format).
 */
function findTraceFiles(directory) {
    return findOtelFilesWithSuffix(directory, "-trace.jsonl");
}

/**
 * Returns all spans from an OTLP trace record as a flat array.
 * @param {Object} record - A raw OTLP JSON record.
 * @returns {Array<Object>} A flat array of span objects across all resource and scope spans.
 */
export function getFlatSpansList(record) {
    let spans = [];
    for (const resourceSpan of record?.resourceSpans ?? []) {
        for (const scopeSpan of resourceSpan.scopeSpans ?? []) {
            for (const span of scopeSpan.spans ?? []) {
                spans.push(span);
            }
        }
    }
    return spans;
}

/**
 * Returns all spans exported to the given trace directory as a flat array.
 * @param {string} directory - The directory path to search in.
 * @returns {Array<Object>} A flat array of every span found across all trace files.
 */
export function getAllSpans(directory) {
    return findTraceFiles(directory).flatMap((file) =>
        readJsonlFile(file.name).flatMap(getFlatSpansList),
    );
}

/**
 * Flattens an OTLP attribute value ({stringValue|intValue|boolValue|doubleValue}) to a JS value.
 */
function otelAttributeValue(value) {
    if (value === undefined || value === null) {
        return undefined;
    }
    return value.stringValue ?? value.intValue ?? value.boolValue ?? value.doubleValue ?? undefined;
}

/**
 * Returns the resource attributes of an OTLP resourceSpan as a flat {key: value} object.
 * @param {Object} resourceSpan
 * @returns {Object}
 */
function getResourceAttributes(resourceSpan) {
    const attributes = {};
    for (const attr of resourceSpan.resource?.attributes ?? []) {
        attributes[attr.key] = otelAttributeValue(attr.value);
    }
    return attributes;
}

/**
 * Returns all spans exported to the given trace directory, each annotated with a `resource` field
 * holding the resource attributes (e.g. `service.name`, `service.instance.id`) of the node that
 * emitted it. This lets callers attribute spans to a specific process even when multiple nodes
 * export into the same directory.
 * @param {string} directory - The directory path to search in.
 * @returns {Array<Object>} A flat array of spans, each with an added `resource` object.
 */
function getAllSpansWithResource(directory) {
    const spans = [];
    for (const file of findTraceFiles(directory)) {
        for (const record of readJsonlFile(file.name)) {
            for (const resourceSpan of record?.resourceSpans ?? []) {
                const resource = getResourceAttributes(resourceSpan);
                for (const scopeSpan of resourceSpan.scopeSpans ?? []) {
                    for (const span of scopeSpan.spans ?? []) {
                        spans.push(Object.assign({resource}, span));
                    }
                }
            }
        }
    }
    return spans;
}

/**
 * Reads every span exported by any of the given nodes, annotated with resource attributes and
 * deduplicated across their (possibly shared) trace directories. Use to inspect a whole cluster's
 * spans regardless of whether nodes export to distinct directories or a shared one.
 * @param {Array<Mongo>} conns - Connections to the nodes.
 * @returns {Array<Object>} A flat, deduplicated array of spans, each with a `resource` object.
 */
export function readClusterSpans(conns) {
    const dirs = new Set(conns.map((conn) => getEffectiveTraceDir(conn)));
    const seen = new Set();
    const spans = [];
    for (const dir of dirs) {
        for (const span of getAllSpansWithResource(dir)) {
            const key = span.traceId + "/" + span.spanId;
            if (!seen.has(key)) {
                seen.add(key);
                spans.push(span);
            }
        }
    }
    return spans;
}

/**
 * Returns a node's OTel `service.instance.id`, which equals its process id. Spans emitted by that
 * node carry this value in `span.resource["service.instance.id"]`. This lets a caller attribute
 * spans to a specific node even in a shared trace directory.
 * @param {Mongo} conn - A connection to the node.
 * @returns {string}
 */
export function getServiceInstanceId(conn) {
    return assert
        .commandWorked(conn.adminCommand({serverStatus: 1}))
        .pid.valueOf()
        .toString();
}

/**
 * Returns true if the span is a trace root (has no parent span).
 * @param {Object} span
 * @returns {boolean}
 */
export function isRootSpan(span) {
    const parent = span.parentSpanId;
    return !parent || /^0*$/.test(parent);
}

/**
 * Returns the `service.instance.id` of the node that emitted a span, as annotated by
 * readClusterSpans().
 * @param {Object} span
 * @returns {string|undefined}
 */
function instanceIdOf(span) {
    return span.resource?.["service.instance.id"];
}

/**
 * Returns true if the given spans (all from one trace, and all for the same command) show a complete
 * mongos->mongod fanout: every shard has an ingress span whose parent is a router egress span, those
 * egress spans are distinct from one another, and they all share one common router parent span (the
 * router's span for the command).
 *
 * @param {Array<Object>} spans - Spans of a single trace, annotated with `resource` (see
 *     readClusterSpans()).
 * @param {Object} options
 * @param {string} options.routerId - `service.instance.id` of the router (mongos).
 * @param {Array<string>} options.shardIds - `service.instance.id` of each shard expected to be
 *     targeted. The fanout is only complete if every one of them is reached.
 * @returns {boolean}
 */
export function showsFullFanout(spans, {routerId, shardIds}) {
    const byId = new Map(spans.map((span) => [span.spanId, span]));

    // For each shard, find the router egress span that parents the shard's ingress span.
    const egressByShard = new Map();
    for (const span of spans) {
        if (!shardIds.includes(instanceIdOf(span))) {
            continue;
        }
        const parent = byId.get(span.parentSpanId);
        if (parent && instanceIdOf(parent) === routerId) {
            egressByShard.set(instanceIdOf(span), parent);
        }
    }
    if (egressByShard.size !== shardIds.length) {
        return false;
    }

    // Each shard must have its own egress span, and all of them must hang off one router span.
    const egressSpans = [...egressByShard.values()];
    const distinctEgressSpans = new Set(egressSpans.map((span) => span.spanId));
    const egressParents = new Set(egressSpans.map((span) => span.parentSpanId));
    if (distinctEgressSpans.size !== shardIds.length || egressParents.size !== 1) {
        return false;
    }
    const commonParent = byId.get([...egressParents][0]);
    return commonParent !== undefined && instanceIdOf(commonParent) === routerId;
}

/**
 * Reads back the trace directory a node is exporting to. The configured value may be overridden by
 * resmoke (see jstests/libs/shardingtest.js), so always read the effective value.
 * @param {Mongo} conn - A connection to the node (mongos or mongod).
 * @returns {string} The effective opentelemetryTraceDirectory.
 */
export function getEffectiveTraceDir(conn) {
    const res = assert.commandWorked(
        conn.adminCommand({getParameter: 1, opentelemetryTraceDirectory: 1}),
    );
    return res.opentelemetryTraceDirectory;
}

export const kFullSamplingStrategy = {
    samplingFactor: 1.0,
    tokenBucketRateLimit: {refillRate: 1000000, maxTokens: NumberInt(1000000)},
};

export const kNoSamplingStrategy = {
    samplingFactor: 0.0,
    tokenBucketRateLimit: {refillRate: 1, maxTokens: NumberInt(1)},
};

// Fields of a sampling config that the server types as `double`. See trace_sampling_parameters.idl.
const kSamplingDoubleFields = new Set(["samplingFactor", "refillRate"]);

/**
 * Serializes a sampling config for use as a *startup* setParameter.
 *
 * The shell serializes object-valued startup setParameters with JSON.stringify (see
 * addOptionsToFullArgs in src/mongo/shell/servers.js), which loses BSON type information: an
 * integral double such as 1.0 is written as `1`, which the server then parses as an int and rejects
 * for a double-typed field. Emitting extended JSON for those fields preserves the type, and
 * returning a string keeps the shell from re-stringifying it.
 *
 * @param {Object} config - An OpenTelemetryTracingSamplingConfig.
 * @returns {string} The config as extended JSON, ready to pass as a startup setParameter.
 */
export function samplingConfigForStartup(config) {
    return JSON.stringify(config, (key, value) =>
        kSamplingDoubleFields.has(key) ? {$numberDouble: String(value)} : value,
    );
}

function fullSamplingOverridesFor(spanNames) {
    return spanNames.map((name) => ({
        spanSelection: {name},
        samplingStrategy: kFullSamplingStrategy,
    }));
}

/**
 * Sets the head-based sampling config of a node. Requires the node to have been started with
 * featureFlagOtelTraceSampling. Any options that are left unset will be reset to default.
 * @param {Mongo} conn - A connection to the node.
 * @param {Object} options
 * @param {boolean} [options.defaultSpans] - If true, raise the rate of spans that are sampled by
 *     default to 1.0. If false (the default), leave `defaultSampling` at the server default.
 * @param {Array<string>} [options.spanNames] - Span (command) names to force-sample at 1.0 via
 *     per-span overrides, regardless of whether they are sampled by default.
 */
export function enableFullSampling(conn, {defaultSpans = false, spanNames = []} = {}) {
    assert(
        defaultSpans || spanNames.length > 0,
        "must sample the default spans, some named spans, or both",
    );
    const config = {};
    if (defaultSpans) {
        config.defaultSampling = kFullSamplingStrategy;
    }
    if (spanNames.length > 0) {
        config.samples = fullSamplingOverridesFor(spanNames);
    }
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, openTelemetryTracingSampling: config}),
    );
}

/**
 * Configures a node to accept all externally-sampled traces, so ingress spans are always kept.
 * @param {Mongo} conn - A connection to the node.
 */
export function acceptAllExternalTraces(conn) {
    assert.commandWorked(
        conn.adminCommand({
            setParameter: 1,
            openTelemetryExternalTracing: {
                tokenBucketRateLimit: {refillRate: 1000000, maxTokens: NumberInt(1000000)},
            },
        }),
    );
}

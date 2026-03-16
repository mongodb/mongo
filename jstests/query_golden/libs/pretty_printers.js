const INDENT_SIZE = 2;

/**
 * Indent a string by the desired indentation level
 */
function indentString(indent) {
    return " ".repeat(indent * INDENT_SIZE);
}

/**
 * Serialize a single join stage to string
 */
function joinStageToString(stage, indent) {
    let result = (function () {
        switch (stage.stage) {
            case "HASH_JOIN_EMBEDDING":
                return "HJ ";
            case "NESTED_LOOP_JOIN_EMBEDDING":
                return "NLJ ";
            case "INDEXED_NESTED_LOOP_JOIN_EMBEDDING":
                return "INLJ ";
            default:
                throw new Error(`Unknown join stage: ${stage.stage}`);
        }
    })();
    result += stage.joinPredicates.join(", ") + "\n";
    result += indentString(indent + 1) + "-> ";
    result += `[${stage.leftEmbeddingField}] `;
    result += joinPlanToString(stage.inputStages[0], indent + 2);
    result += indentString(indent + 1) + "-> ";
    result += `[${stage.rightEmbeddingField}] `;
    result += joinPlanToString(stage.inputStages[1], indent + 2);
    return result;
}

/**
 *  Serialize a complete join plan to string
 */
export function joinPlanToString(stage, indent = 0) {
    let result = "";
    let filter = stage.filter && Object.keys(stage.filter).length > 0 ? JSON.stringify(stage.filter) + " " : "";

    switch (stage.stage) {
        case "HASH_JOIN_EMBEDDING":
        case "NESTED_LOOP_JOIN_EMBEDDING":
        case "INDEXED_NESTED_LOOP_JOIN_EMBEDDING":
            result += joinStageToString(stage, indent);
            break;
        case "COLLSCAN":
            result += `COLLSCAN: ${stage.nss} ${filter}\n`;
            break;
        case "FETCH":
            result += `FETCH: ${stage.nss} ${filter}\n`;
            result += indentString(indent + 1) + "-> ";
            result += joinPlanToString(stage.inputStage, indent + 2);
            break;

        case "IXSCAN":
        case "EXPRESS_IXSCAN":
            result += `${stage.stage}: ${stage.nss} ${filter}${stage.indexName} ${JSON.stringify(stage.indexBounds)}\n`;
            break;
        case "INDEX_PROBE_NODE":
            result += `INDEX_PROBE_NODE: ${stage.nss} ${filter}${stage.indexName}\n`;
            break;
        case "PROJECTION_SIMPLE":
        case "SUBPLAN":
            result += `${stage.stage}\n`;
            result += indentString(indent + 1) + "-> ";
            result += joinPlanToString(stage.inputStage, indent + 2);
            break;
        case "OR":
            result += `${stage.stage}\n`;
            result += indentString(indent + 1) + "-> ";
            result += joinPlanToString(stage.inputStages[0], indent + 2);
            result += indentString(indent + 1) + "-> ";
            result += joinPlanToString(stage.inputStages[1], indent + 2);
            break;
        default:
            throw new Error(`Unknown stage: ${stage.stage}`);
    }
    return result;
}

/**
 * Return a multiline string in a JSON-compatible format.
 *
 * JSON does not natively support multiline strings without the use of \n, so
 * the only option here is to convert the string into an array so that each
 * line can be represented as a separate element and printed on a separate line of the output.
 *
 * This makes the output both human-readable and a valid JSON
 */
export function jsonifyMultilineString(str) {
    // Replace double quotes with single quotes in order to avoid escaping them,
    // since escaping reduces readability
    const replacedQuotes = str.replace(/"/g, "'");

    const lines = replacedQuotes.split(/\r?\n/);

    // Wrap each line in quotes and separate with commas, with no trailing comma
    return lines
        .map((line, i) => (i === lines.length - 1 ? JSON.stringify(line) : JSON.stringify(line) + ","))
        .join("\n");
}

/**
 * Return a pipeline where each of $match, $lookup, $unwind starts on a new line.
 *
 * This increases the readability of joins while keeping nested $and, $or, etc. on a single line
 */
export function newlineBeforeEachStage(str) {
    return str.replace(/(?=\{"\$(match|lookup|unwind)")/g, "\n");
}

/**
 * Reduces a query plan in-place to a more compact representation by retaining only the fields
 * that pertain to stage names, filtering and index usage. This representation is suitable for
 * CBR golden tests such as plan_stability.js where we want to record the general shape of the
 * query plan on a single line.
 */
export function trimPlanToStagesAndIndexes(obj) {
    const fieldsToKeep = ["stage", "inputStage", "inputStages", "indexName", "indexBounds", "filter"];

    if (typeof obj !== "object" || obj === null) {
        return obj;
    }
    for (let key in obj) {
        if (!Array.isArray(obj) && !fieldsToKeep.includes(key)) {
            delete obj[key];
        } else if (key == "filter") {
            // Preserve the presence of a filter without retaining the actual expression
            obj[key] = true;
        } else {
            if (typeof obj[key] === "object" && obj[key] !== null && key !== "indexBounds") {
                trimPlanToStagesAndIndexes(obj[key]);
            }
        }
    }
    return obj;
}

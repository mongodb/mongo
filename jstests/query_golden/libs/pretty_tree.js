/**
 * Generic tree-printer for golden tests.
 */
import {code} from "jstests/libs/query/pretty_md.js";

function defaultNodeToString(node) {
    return tojsononeline(node);
}

function defaultGetChildrenForNode(node) {
    return node.children || [];
}

const kWhitespaceSeparator = "  ";
const kEdge = "|";
function prefix(numEdges) {
    return (kWhitespaceSeparator + kEdge).repeat(numEdges + 1);
}

function formatMultilineNodeStr(nodeStr, numEdges) {
    const lines = nodeStr.split("\n");
    const edges = "\n" + prefix(numEdges - 1) + kWhitespaceSeparator;
    const prevEdges = prefix(numEdges) + "\n";
    return prevEdges + prefix(numEdges - 1) + kWhitespaceSeparator + lines.join(edges);
}

function formatNodeForTree(nodeStr, depth, numEdges) {
    if (depth == 0) {
        // Root node.
        return nodeStr + "\n";
    }

    return formatMultilineNodeStr(nodeStr, numEdges) + "\n";
}

/**
 * Main entry point for generating a pretty-printed tree string.
 */
export function prettyTreeString(
    node,
    nodeLambda = defaultNodeToString,
    getChildrenForNode = defaultGetChildrenForNode,
    depth = 0,
    numEdges = 0,
) {
    // Print root.
    const printedNode = nodeLambda(node);
    let out = formatNodeForTree(printedNode, depth, numEdges);

    // Traverse tree- we print the last child first (highest in the tree).
    const children = getChildrenForNode(node);
    for (let i = children.length - 1; i >= 0; i--) {
        // Increase distance from left & number of edges to print by index of child.
        out += prettyTreeString(children[i], nodeLambda, getChildrenForNode, depth + 1, numEdges + i);
    }
    return out;
}

/**
 * Main entry point for markdown-formatted tree string.
 */
export function prettyPrintTree(
    node,
    nodeLambda = defaultNodeToString,
    getChildrenForNode = defaultGetChildrenForNode,
) {
    code(prettyTreeString(node, nodeLambda, getChildrenForNode), "");
}

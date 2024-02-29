/**
 * A class representing a directed graph.
 */
export function Graph() {
    if (!(this instanceof Graph)) {
        return new Graph();
    }

    const nodes = new Set();
    const adjList = new BSONAwareMap();

    this.addEdge = function addEdge(fromNode, toNode) {
        nodes.add(fromNode);
        nodes.add(toNode);

        let neighbors = adjList.get(fromNode);
        if (!(neighbors instanceof Set)) {
            neighbors = new Set();
            adjList.put(fromNode, neighbors);
        }
        neighbors.add(toNode);
    };

    const State = {
        kNotYetVisited: Symbol('not yet visited'),
        kVisitingInProgress: Symbol('visiting in progress'),
        kAlreadyVisited: Symbol('already visited'),
    };

    /**
     * If a cycle exists in this graph, then this function returns an array of the nodes comprising
     * the cycle. The returned array is guaranteed to contain at least two elements, where the first
     * and last elements refer to the same node.
     *
     * If a cycle doesn't exist in this graph, then this function returns an empty array.
     *
     * The algorithm implemented here for detecting whether a cycle exists in the graph is adapted
     * from the algorithm for using a depth-first search to determine whether the graph permits a
     * topological sort. Note that a topological ordering is possible if and only if the graph is
     * acyclic.
     *
     * References:
     *   - https://en.wikipedia.org/wiki/Topological_sorting#Depth-first_search
     *   - http://www.cs.cornell.edu/courses/cs2112/2012sp/lectures/lec24/lec24-12sp.html
     */
    this.findCycle = function findCycle() {
        const state = new BSONAwareMap();
        for (let node of nodes) {
            state.put(node, State.kNotYetVisited);
        }

        function doDepthFirstSearch(node) {
            {
                const nodeState = state.get(node);
                if (nodeState !== State.kNotYetVisited) {
                    throw new Error('Found node ' + tojsononeline(node) +
                                    ' unexpectedly in state ' + nodeState.toString());
                }
            }

            state.put(node, State.kVisitingInProgress);

            const neighbors = adjList.get(node) || [];
            for (let otherNode of neighbors) {
                const otherNodeState = state.get(otherNode);
                if (otherNodeState === State.kAlreadyVisited) {
                    // We've already explored all neighbors of 'otherNode'. Since we are currently
                    // in the process of visiting 'node', it must be the case there doesn't exist a
                    // path from 'otherNode' to 'node'. There is therefore no cycle containing both
                    // 'node' and 'otherNode'.
                    continue;
                }

                if (otherNodeState === State.kVisitingInProgress) {
                    // We're currently in the process of exploring all neighbors of 'otherNode'.
                    // Since we are currently in the process of visiting 'node', it must be the case
                    // that there exists a path from 'otherNode' to 'node'. There is therefore a
                    // cycle containing both 'node' and 'otherNode'.
                    return [node, otherNode];
                }

                const result = doDepthFirstSearch(otherNode);
                if (result.length > 1) {
                    // A cycle has been detected during the recursive call to doDepthFirstSearch().
                    // Unless we've already closed the loop, the (node, otherNode) edge must be part
                    // of it. Note that we use friendlyEqual() to match the definition of sameness
                    // as the mongo shell's BSONAwareMap type.
                    if (!friendlyEqual(result[0], result[result.length - 1])) {
                        result.unshift(node);
                    }
                    return result;
                }
            }

            state.put(node, State.kAlreadyVisited);
            return [];
        }

        for (let node of nodes) {
            if (state.get(node) === State.kAlreadyVisited) {
                // We've already explored all paths from 'node' by starting from one of its
                // ancestors and didn't find a cycle. There is therefore no cycle involving 'node'
                // so we move onto another node.
                continue;
            }

            const result = doDepthFirstSearch(node);
            if (result.length > 0) {
                return result;
            }
        }

        return [];
    };
}

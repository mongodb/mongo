import { createStore, combineReducers } from "redux";
import { nodes } from "./nodes";
import { graphFiles } from "./graphFiles";
import { counts } from "./counts";
import { nodeInfo } from "./nodeInfo";
import { loading } from "./loading";
import { links } from "./links";
import { graphData } from "./graphData";
import { findNode } from "./findNode";

export const initialState = {
  loading: false,
  graphFiles: [
    // {id: 0, value: 'graphfile.graphml', version: 1, git: '1234567', selected: false}
  ],
  nodes: [
    {
      id: 0,
      node: "test/test1.so",
      name: "test1",
      check: "checkbox",
      selected: false,
    },
    {
      id: 1,
      node: "test/test2.so",
      name: "test2",
      check: "checkbox",
      selected: false,
    },
  ],
  links: [{ source: "test/test1.so", target: "test/test2.so" }],
  graphData: {
    nodes: [
      // {id: 'test/test1.so', name: 'test1.so'},
      // {id: 'test/test2.so', name: 'test2.so'}
    ],
    links: [
      // {source: 'test/test1.so', target: 'test/test2.so'}
    ],
  },
  counts: [{ id: 0, type: "node2", value: 0 }],
  findNode: "",
  nodeInfo: [
    {
      id: 0,
      node: "test/test.so",
      name: "test",
      attribs: [{ name: "test", value: "test" }],
      dependers: [{ node: "test/test3.so", symbols: [] }],
      dependencies: [{ node: "test/test2.so", symbols: [] }],
    },
  ],
};

export const getLoading = (state) => {
  return { loading: state };
};

export const getGraphFiles = (state) => {
  return {
    loading: state.loading,
    graphFiles: state.graphFiles,
  };
};

export const getNodeInfos = (state) => {
  return {
    nodeInfos: state.nodeInfo,
  };
};

export const getCounts = (state) => {
  const counts = state.counts;
  return {
    counts: state.counts,
  };
};

export const getRows = (state) => {
  return {
    rowCount: state.nodes.length,
    rowGetter: ({ index }) => state.nodes[index],
    checkBox: ({ index }) => state.nodes[index].selected,
    nodes: state.nodes,
  };
};

export const getSelected = (state) => {
  return {
    selectedNodes: state.nodes.filter((node) => node.selected),
    selectedEdges: [],
    loading: state.loading,
  };
};

export const getNodes = (state) => {
  return {
    nodes: state.nodes,
    loading: state.loading,
  };
};

export const getGraphData = (state) => {
  return {
    nodes: state.nodes,
    graphData: state.graphData,
    loading: state.loading,
    findNode: state.findNode,
  };
};

export const getFullState = (state) => {
  return { state };
};

const store = createStore(
  combineReducers({
    nodes,
    counts,
    nodeInfo,
    graphFiles,
    loading,
    links,
    graphData,
    findNode,
  }),
  initialState
);
export default store;

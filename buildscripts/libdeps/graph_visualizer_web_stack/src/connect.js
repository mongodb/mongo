import React from "react";
import io from "socket.io-client";
import { connect as reduxConnect } from "react-redux";

import { setNodes, updateCheckboxes } from "./redux/nodes";
import { setCounts } from "./redux/counts";
import { setNodeInfos } from "./redux/nodeInfo";
import { setGraphFiles, selectGraphFile } from "./redux/graphFiles";
import { setLoading } from "./redux/loading";
import { addLinks } from "./redux/links";
import { setGraphData } from "./redux/graphData";

const { REACT_APP_API_URL } = process.env;

export const socket = io.connect(REACT_APP_API_URL, {
  reconnection: true,
  transports: ["websocket"],
});

const SocketConnection = ({
  setNodes,
  updateCheckboxes,
  setCounts,
  setGraphFiles,
  setLoading,
  selectGraphFile,
  addLinks,
  setGraphData,
  setNodeInfos,
}) => {
  React.useEffect(() => {
    fetch(REACT_APP_API_URL + "/graph_files")
      .then((res) => res.json())
      .then((data) => {
        setGraphFiles(data.graph_files);
      })
      .catch((err) => {
        /* eslint-disable no-console */
        console.log("Error Reading data " + err);
      });

    socket.on("other_hash_selected", (incomingData) => {
      selectGraphFile({
        hash: incomingData.hash,
        selected: incomingData.selected,
      });
    });

    socket.on("graph_nodes", (incomingData) => {
      setLoading(false);
      setNodes(
        incomingData.graphData.nodes.map((node, index) => {
          return {
            id: index,
            node: node.id,
            name: node.name,
            check: "checkbox",
            selected: false,
          };
        })
      );
      addLinks(incomingData.graphData.links);
    });

    socket.on("graph_data", (incomingData) => {
      if (incomingData.graphData) {
        setGraphData(incomingData.graphData);
      }
      if (incomingData.selectedNodes) {
        updateCheckboxes(
          incomingData.selectedNodes.map((node, index) => {
            return { node: node, value: true };
          })
        );
      }
    });

    socket.on("graph_results", (incomingData) => {
      setCounts(incomingData);
    });

    socket.on("node_infos", (incomingData) => {
      setNodeInfos(incomingData.nodeInfos);
    });
  }, []);

  return null;
};

export default reduxConnect(null, {
  setNodes,
  updateCheckboxes,
  setCounts,
  setNodeInfos,
  setGraphFiles,
  setLoading,
  selectGraphFile,
  addLinks,
  setGraphData,
})(SocketConnection);

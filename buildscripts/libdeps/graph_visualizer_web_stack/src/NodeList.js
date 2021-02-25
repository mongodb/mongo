import React from "react";

import { connect } from "react-redux";
import { getNodes } from "./redux/store";
import { setFindNode } from "./redux/findNode";
import { socket } from "./connect";

import DataGrid from "./DataGrid";
import LoadingBar from "./LoadingBar";

const columns = [
  { dataKey: "check", label: "Selected", width: 70 },
  { dataKey: "name", label: "Name", width: 200 },
  { id: "ID", dataKey: "node", label: "Node", width: 200 },
];

const NodeList = ({ nodes, loading, setFindNode }) => {
  function handleCheckBoxes(rowIndex, event) {
    socket.emit("row_selected", {
      data: { node: nodes[rowIndex].node, name: nodes[rowIndex].name },
      isSelected: event.target.checked,
    });
  }

  function handleRowClick(event) {
    setFindNode(event.target.textContent);
  }

  return (
    <LoadingBar loading={loading} height={"95%"}>
      <DataGrid
        rows={nodes}
        columns={columns}
        rowHeight={30}
        headerHeight={35}
        onNodeClicked={handleRowClick}
        onRowSelect={handleCheckBoxes}
      />
    </LoadingBar>
  );
};

export default connect(getNodes, { setFindNode })(NodeList);

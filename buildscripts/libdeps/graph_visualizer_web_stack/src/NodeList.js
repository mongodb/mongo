import React from "react";

import { connect } from "react-redux";
import { getNodes } from "./redux/store";
import { setFindNode } from "./redux/findNode";

import DataGrid from "./DataGrid";
import LoadingBar from "./LoadingBar";
import TextField from "@material-ui/core/TextField";

import { setNodes } from "./redux/nodes";
import { addLinks } from "./redux/links";
import { setLoading } from "./redux/loading";
import { setListSearchTerm } from "./redux/listSearchTerm";

const columns = [
  { dataKey: "check", label: "Selected", width: 70 },
  { dataKey: "name", label: "Name", width: 200 },
  { id: "ID", dataKey: "node", label: "Node", width: 200 },
];

const NodeList = ({ selectedGraph, nodes, loading, setFindNode, setNodes, addLinks, setLoading, setListSearchTerm }) => {

  React.useEffect(() => {
    let gitHash = selectedGraph;
    fetch('/api/graphs/' + gitHash + '/nodes')
      .then(response => response.json())
      .then(data => {
        setNodes(data.nodes.map((node, index) => {
          return {
            id: index,
            node: node,
            name: node.substring(node.lastIndexOf('/') + 1),
            check: "checkbox",
            selected: false,
          };
        }));
        addLinks(data.links);
        setLoading(false);
      });
  }, [selectedGraph]);

  function handleRowClick(event) {
    setFindNode(event.target.textContent);
  }

  function handleSearchTermChange(event) {
    setListSearchTerm(event.target.value);
  }

  return (
    <LoadingBar loading={loading} height={"95%"}>
      <TextField
        fullWidth
        onChange={handleSearchTermChange}
        onClick={(event)=> event.target.select()}
        label="Search for Node"
      />
      <DataGrid
        rows={nodes}
        columns={columns}
        rowHeight={30}
        headerHeight={35}
        onNodeClicked={handleRowClick}
      />
    </LoadingBar>
  );
};

export default connect(getNodes, { setFindNode, setNodes, addLinks, setLoading, setListSearchTerm })(NodeList);
import React from "react";

import { connect } from "react-redux";
import { getNodes } from "./redux/store";
import { setFindNode } from "./redux/findNode";

import DataGrid from "./DataGrid";
import LoadingBar from "./LoadingBar";
import TextField from "@material-ui/core/TextField";

import { setNodes, updateCheckbox, updateSelected } from "./redux/nodes";
import { setGraphData } from "./redux/graphData";
import { addLinks } from "./redux/links";
import { setLoading } from "./redux/loading";
import { setListSearchTerm } from "./redux/listSearchTerm";
import { Button, Autocomplete, Grid } from "@material-ui/core";

const columns = [
  { dataKey: "check", label: "Selected", width: 70 },
  { dataKey: "name", label: "Name", width: 200 },
  { id: "ID", dataKey: "node", label: "Node", width: 200 },
];

const NodeList = ({ selectedGraph, nodes, searchedNodes, loading, setFindNode, setNodes, addLinks, setLoading, setListSearchTerm, updateCheckbox, updateSelected, setGraphData}) => {
  const [searchPath, setSearchPath] = React.useState('');

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
    setSearchPath(null);
    setListSearchTerm('');
  }, [selectedGraph]);

  function newGraphData() {
    let gitHash = selectedGraph;
    let postData = {
        "selected_nodes": nodes.filter(node => node.selected == true).map(node => node.node)
    };
    fetch('/api/graphs/' + gitHash + '/d3', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(postData)
    })
      .then(response => response.json())
      .then(data => {
        setGraphData(data.graphData);
      });
    fetch('/api/graphs/' + gitHash + '/nodes/details', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(postData)
    })
      .then(response => response.json())
      .then(data => {
        setNodeInfos(data.nodeInfos);
      });
  }

  function nodePaths() {
    const paths = nodes.map(node => node.node.substring(0, node.node.lastIndexOf('/') + 1));
    return [...new Set(paths)];
  }

  function handleRowClick(event) {
    setFindNode(event.target.textContent);
  }

  function handleSelectAll(event) {
    searchedNodes.forEach(node => {
      updateCheckbox({ node: node.id, value: "flip" });
      updateSelected({ index: node.id, value: true });
    });
    newGraphData();
  }

  function handleDeselectAll(event) {
    searchedNodes.forEach(node => {
      updateCheckbox({ node: node.id, value: "flip" });
      updateSelected({ index: node.id, value: false });
    });
    newGraphData();
  }

  function handleSearchTermChange(event, newTerm) {
    if (newTerm == null) {
      setSearchPath('');
      setListSearchTerm(newTerm);
    } else {
      setSearchPath(newTerm);
      setListSearchTerm(newTerm);
    }
  }

  return (
    <LoadingBar loading={loading} height={"95%"}>
      <Grid container spacing={2}>
        <Grid item xs={12}/>
        <Grid item xs={12}>
          <Autocomplete
            fullWidth
            freeSolo
            ListboxProps={{ style: { maxHeight: "9rem" } }}
            value={searchPath}
            onInputChange={handleSearchTermChange}
            onChange={handleSearchTermChange}
            options={nodePaths()}
            renderInput={(params) => <TextField {...params}
              label="Search by Path or Name"
              variant="outlined"
              onClick={(event) => event.target.select()}
              />}
          />
        </Grid>
        <Grid item xs={12}>
          <Grid
            container
            direction="row"
            justifyContent="center"
            spacing={4}
          >
            <Grid item>
              <Button
                variant="contained"
                onClick={handleSelectAll}
              >
                Select All
              </Button>
            </Grid>
            <Grid item>
              <Button
                variant="contained"
                onClick={handleDeselectAll}
              >
                Deselect All
              </Button>
            </Grid>
          </Grid>
        </Grid>
        <Grid item xs={12}/>
      </Grid>
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

export default connect(getNodes, { setFindNode, setNodes, addLinks, setLoading, setListSearchTerm, updateCheckbox, updateSelected, setGraphData })(NodeList);
import React, { useRef, useEffect } from "react";
import * as THREE from "three";
import { connect } from "react-redux";
import ForceGraph2D from "react-force-graph-2d";
import ForceGraph3D from "react-force-graph-3d";
import SwitchComponents from "./SwitchComponent";
import Button from "@material-ui/core/Button";
import TextField from "@material-ui/core/TextField";
import FormControlLabel from "@material-ui/core/FormControlLabel";
import Checkbox from "@material-ui/core/Checkbox";

import theme from "./theme";
import { getGraphData } from "./redux/store";
import { updateCheckbox } from "./redux/nodes";
import { setFindNode } from "./redux/findNode";
import { setGraphData } from "./redux/graphData";
import { setNodeInfos } from "./redux/nodeInfo";
import { setLinks } from "./redux/links";
import { setLinksTrans } from "./redux/linksTrans";
import { setShowTransitive } from "./redux/showTransitive";
import LoadingBar from "./LoadingBar";

const handleFindNode = (node_value, graphData, activeComponent, forceRef) => {
  var targetNode = null;
  if (graphData) {
    for (var i = 0; i < graphData.nodes.length; i++) {
      var node = graphData.nodes[i];
      if (node.name == node_value || node.id == node_value) {
        targetNode = node;
        break;
      }
    }
    if (targetNode != null) {
      if (activeComponent == "3D") {
        if (forceRef.current != null) {
          forceRef.current.centerAt(targetNode.x, targetNode.y, 2000);
          forceRef.current.zoom(6, 1000);
        }
      } else {
        const distance = 100;
        const distRatio =
          1 + distance / Math.hypot(targetNode.x, targetNode.y, targetNode.z);
        if (forceRef.current != null) {
          forceRef.current.cameraPosition(
            {
              x: targetNode.x * distRatio,
              y: targetNode.y * distRatio,
              z: targetNode.z * distRatio,
            }, // new position
            targetNode, // lookAt ({ x, y, z })
            3000 // ms transition duration
          );
        }
      }
    }
  }
};

const DrawGraph = ({
  size,
  graphData,
  nodes,
  links,
  loading,
  graphPaths,
  updateCheckbox,
  findNode,
  setFindNode,
  setGraphData,
  setNodeInfos,
  selectedGraph,
  setLinks,
  setLinksTrans,
  setShowTransitive,
  showTransitive
}) => {
  const [activeComponent, setActiveComponent] = React.useState("2D");
  const [pathNodes, setPathNodes] = React.useState({});
  const [pathEdges, setPathEdges] = React.useState([]);
  const forceRef = useRef(null);

  const PARTICLE_SIZE = 5;

  React.useEffect(() => {
    handleFindNode(findNode, graphData, activeComponent, forceRef);
    setFindNode("");
  }, [findNode, graphData, activeComponent, forceRef]);

  React.useEffect(() => {
    newGraphData();
  }, [showTransitive]);

  const selectedEdge = links.filter(link => link.selected == true)[0];
  const selectedNodes = nodes.filter(node => node.selected == true).map(node => node.node);

  React.useEffect(() => {
    setPathNodes({ fromNode: graphPaths.fromNode, toNode: graphPaths.toNode });
    var paths = Array();
    for (var path = 0; path < graphPaths.paths.length; path++) {
      var pathArr = Array();
      for (var i = 0; i < graphPaths.paths[path].length; i++) {
        if (i == 0) {
          continue;
        }
        pathArr.push({
          source: graphPaths.paths[path][i - 1],
          target: graphPaths.paths[path][i],
        });
      }
      paths.push(pathArr);
    }
    setPathEdges(paths);
  }, [graphPaths]);

  React.useEffect(() => {
    if (forceRef.current != null) {
      if (activeComponent == '3D'){
        forceRef.current.d3Force("charge").strength(-2000);
      }
      else {
        forceRef.current.d3Force("charge").strength(-10000);
      }

    }
  }, [forceRef.current, activeComponent]);

  function newGraphData() {
    let gitHash = selectedGraph;
    if (gitHash) {
      let postData = {
          "selected_nodes": nodes.filter(node => node.selected == true).map(node => node.node),
          "transitive_edges": showTransitive
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
          setLinks(data.graphData.links);
          setLinksTrans(data.graphData.links_trans);
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
  }

  const paintRing = React.useCallback(
    (node, ctx) => {
      // add ring just for highlighted nodes
      ctx.beginPath();
      ctx.arc(node.x, node.y, 7 * 1.4, 0, 2 * Math.PI, false);
      if (node.id == pathNodes.fromNode) {
        ctx.fillStyle = "blue";
      } else if (node.id == pathNodes.toNode) {
        ctx.fillStyle = "red";
      } else {
        ctx.fillStyle = "green";
      }
      ctx.fill();
    },
    [pathNodes]
  );

  function colorNodes(node) {
    switch (node.type) {
      case "SharedLibrary":
        return "#e6ed11"; // yellow
      case "Program":
        return "#1120ed"; // blue
      case "shim":
        return "#800303"; // dark red
      default:
        return "#5a706f"; // grey
    }
  }

  function isSameEdge(edgeA, edgeB) {
    if (edgeA.source.id && edgeA.target.id) {
      if (edgeB.source.id && edgeB.target.id) {
        return (edgeA.source.id == edgeB.source.id &&
                edgeA.target.id == edgeB.target.id);
      }
    }
    if (edgeA.source == edgeB.source &&
        edgeA.target == edgeB.target) {
          return true;
    }
    return false;
  }

  return (
    <LoadingBar loading={loading} height={"100%"}>
      <Button
        onClick={() => {
          if (activeComponent == "2D") {
            setActiveComponent("3D");
          } else {
            setActiveComponent("2D");
          }
        }}
      >
        {activeComponent}
      </Button>
      <TextField
        size="small"
        label="Find Node"
        onChange={(event) => {
          handleFindNode(
            event.target.value,
            graphData,
            activeComponent,
            forceRef
          );
        }}
      />
      <FormControlLabel
        style={{ marginInline: 5 }}
        control={<Checkbox
                    style={{ marginInline: 10 }}
                    checked={ showTransitive }
                    onClick={ () => setShowTransitive(!showTransitive) }
                />}
        label="Show Viewable Transitive Edges"
      />
      <SwitchComponents active={activeComponent}>
        <ForceGraph2D
          name="3D"
          width={size}
          dagMode="radialout"
          dagLevelDistance={50}
          graphData={graphData}
          ref={forceRef}
          nodeColor={colorNodes}
          nodeOpacity={1}
          backgroundColor={theme.palette.secondary.dark}
          linkDirectionalArrowLength={6}
          linkDirectionalArrowRelPos={1}
          linkDirectionalParticles={(d) => {
            if (graphPaths.selectedPath >= 0) {
              for (
                var i = 0;
                i < pathEdges[graphPaths.selectedPath].length;
                i++
              ) {
                if (
                  pathEdges[graphPaths.selectedPath][i].source == d.source.id &&
                  pathEdges[graphPaths.selectedPath][i].target == d.target.id
                ) {
                  return PARTICLE_SIZE;
                }
              }
            }
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, d)) {
                return PARTICLE_SIZE;
              }
            }
            return 0;
          }}
          linkDirectionalParticleSpeed={(d) => {
            return 0.01;
          }}
          nodeCanvasObjectMode={(node) => {
            if (selectedNodes.includes(node.id)) {
              return "before";
            }
          }}
          linkLineDash={(d) => { 
            if (d.data.direct) {
              return [];
            }
            return [5, 3];
          }}
          linkColor={(d) => {
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, d)) {
                  return "#ED7811";
              }
            }
            if (graphPaths.selectedPath >= 0) {
              for (
                var i = 0;
                i < pathEdges[graphPaths.selectedPath].length;
                i++
              ) {
                if (
                  pathEdges[graphPaths.selectedPath][i].source == d.source.id &&
                  pathEdges[graphPaths.selectedPath][i].target == d.target.id
                ) {
                  return "#12FF19";
                }
              }
            }
            return "#FAFAFA";
          }}
          linkDirectionalParticleWidth={6}
          linkWidth={(d) => {
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, d)) {
                  return 2;
              }
            }
            if (graphPaths.selectedPath >= 0) {
              for (
                var i = 0;
                i < pathEdges[graphPaths.selectedPath].length;
                i++
              ) {
                if (
                  pathEdges[graphPaths.selectedPath][i].source == d.source.id &&
                  pathEdges[graphPaths.selectedPath][i].target == d.target.id
                ) {
                  return 2;
                }
              }
            }
            return 1;
          }}
          onLinkClick={(link, event) => {
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, link)) {
                      setLinks(
                        links.map((temp_link) => {
                          temp_link.selected = false;
                          return temp_link;
                        })
                      );
                      return;
                }
            }
            setLinks(
              links.map((temp_link, index) => {
                if (index == link.index) {
                  temp_link.selected = true;
                } else {
                  temp_link.selected = false;
                }
                return temp_link;
              })
            );
          }}
          nodeRelSize={7}
          nodeCanvasObject={paintRing}
          onNodeClick={(node, event) => {
            updateCheckbox({ node: node.id, value: "flip" });
            newGraphData();
          }}
        />
        <ForceGraph3D
          name="2D"
          width={size}
          dagMode="radialout"
          graphData={graphData}
          nodeColor={colorNodes}
          nodeOpacity={1}
          nodeThreeObject={(node) => {
            if (!selectedNodes.includes(node.id)) {
              return new THREE.Mesh(
                new THREE.SphereGeometry(5, 5, 5),
                new THREE.MeshLambertMaterial({
                  color: colorNodes(node),
                  transparent: true,
                  opacity: 0.2,
                })
              );
            }
          }}
          onNodeClick={(node, event) => {
            updateCheckbox({ node: node.id, value: "flip" });
            newGraphData();
          }}
          linkColor={(d) => {
            if (graphPaths.selectedPath >= 0) {
              for (
                var i = 0;
                i < pathEdges[graphPaths.selectedPath].length;
                i++
              ) {
                if (
                  pathEdges[graphPaths.selectedPath][i].source == d.source.id &&
                  pathEdges[graphPaths.selectedPath][i].target == d.target.id
                ) {
                  return "#12FF19";
                }
              }
            }
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, d)) {
                  return "#ED7811";
              }
            }
            if (d.data.direct == false) {
              return "#303030";
            }
            return "#FFFFFF";
          }}
          linkDirectionalParticleWidth={7}
          linkWidth={(d) => {
            if (graphPaths.selectedPath >= 0) {
              for (
                var i = 0;
                i < pathEdges[graphPaths.selectedPath].length;
                i++
              ) {
                if (
                  pathEdges[graphPaths.selectedPath][i].source == d.source.id &&
                  pathEdges[graphPaths.selectedPath][i].target == d.target.id
                ) {
                  return 3;
                }
              }
            }
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, d)) {
                return 3;
              }
            }
            return 1;
          }}
          linkDirectionalParticles={(d) => {
            if (graphPaths.selectedPath >= 0) {
              for (
                var i = 0;
                i < pathEdges[graphPaths.selectedPath].length;
                i++
              ) {
                if (
                  pathEdges[graphPaths.selectedPath][i].source == d.source.id &&
                  pathEdges[graphPaths.selectedPath][i].target == d.target.id
                ) {
                  return PARTICLE_SIZE;
                }
              }
            }
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, d)) {
                  return PARTICLE_SIZE;
              }
            }
            return 0;
          }}
          linkDirectionalParticleSpeed={(d) => {
            return 0.01;
          }}
          linkDirectionalParticleResolution={10}
          linkOpacity={0.6}
          onLinkClick={(link, event) => {
            if (selectedEdge) {
              if (isSameEdge(selectedEdge, link)) {
                    setLinks(
                      links.map((temp_link) => {
                        temp_link.selected = false;
                        return temp_link;
                      })
                    );
                    return;
              }
            }
            setLinks(
              links.map((temp_link, index) => {
                if (index == link.index) {
                  temp_link.selected = true;
                } else {
                  temp_link.selected = false;
                }
                return temp_link;
              })
            );
          }}
          nodeRelSize={7}
          backgroundColor={theme.palette.secondary.dark}
          linkDirectionalArrowLength={3.5}
          linkDirectionalArrowRelPos={1}
          ref={forceRef}
        />
      </SwitchComponents>
    </LoadingBar>
  );
};

export default connect(getGraphData, { setFindNode, updateCheckbox, setGraphData, setNodeInfos, setLinks, setLinksTrans, setShowTransitive })(
  DrawGraph
);

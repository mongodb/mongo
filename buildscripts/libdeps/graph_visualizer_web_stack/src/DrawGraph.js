import React, { useRef, useEffect } from "react";
import * as THREE from "three";
import { connect } from "react-redux";
import ForceGraph2D from "react-force-graph-2d";
import ForceGraph3D from "react-force-graph-3d";
import SwitchComponents from "./SwitchComponent";
import Button from "@material-ui/core/Button";
import TextField from "@material-ui/core/TextField";

import theme from "./theme";
import { socket } from "./connect";
import { getGraphData } from "./redux/store";
import { updateCheckbox } from "./redux/nodes";
import { setFindNode } from "./redux/findNode";
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
  loading,
  findNode,
  setFindNode,
}) => {
  const [activeComponent, setActiveComponent] = React.useState("2D");
  const [selectedNodes, setSelectedNodes] = React.useState([]);
  const forceRef = useRef(null);

  React.useEffect(() => {
    handleFindNode(findNode, graphData, activeComponent, forceRef);
    setFindNode("");
  }, [findNode, graphData, activeComponent, forceRef]);

  React.useEffect(() => {
    setSelectedNodes(
      nodes.map((node) => {
        if (node.selected) {
          return node.node;
        }
      })
    );
  }, [nodes]);

  React.useEffect(() => {
    if (forceRef.current != null) {
      forceRef.current.d3Force("charge").strength(-1400);
    }
  }, [forceRef.current]);

  const paintRing = React.useCallback((node, ctx) => {
    // add ring just for highlighted nodes
    ctx.beginPath();
    ctx.arc(node.x, node.y, 4 * 1.4, 0, 2 * Math.PI, false);
    ctx.fillStyle = "green";
    ctx.fill();
  });

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
      <SwitchComponents active={activeComponent}>
        <ForceGraph2D
          name="3D"
          width={size}
          dagMode="radialout"
          graphData={graphData}
          ref={forceRef}
          nodeColor={colorNodes}
          nodeOpacity={1}
          backgroundColor={theme.palette.secondary.dark}
          linkDirectionalArrowLength={6}
          linkDirectionalArrowRelPos={1}
          nodeCanvasObjectMode={(node) => {
            if (selectedNodes.includes(node.id)) {
              return "before";
            }
          }}
          nodeCanvasObject={paintRing}
          onNodeClick={(node, event) => {
            updateCheckbox(node.id);
            socket.emit("row_selected", {
              data: { node: node.id, name: node.name },
              isSelected: !selectedNodes.includes(node.id),
            });
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
            updateCheckbox(node.id);
            socket.emit("row_selected", {
              data: { node: node.id, name: node.name },
              isSelected: !selectedNodes.includes(node.id),
            });
          }}
          backgroundColor={theme.palette.secondary.dark}
          linkDirectionalArrowLength={3.5}
          linkDirectionalArrowRelPos={1}
          ref={forceRef}
        />
      </SwitchComponents>
    </LoadingBar>
  );
};

export default connect(getGraphData, { setFindNode })(DrawGraph);

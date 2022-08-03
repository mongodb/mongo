import React, { useRef, useEffect, useState } from "react";
import { connect } from "react-redux";
import Tooltip from "@material-ui/core/Tooltip";
import Fade from "@material-ui/core/Fade";
import Box from "@material-ui/core/Box";
import IconButton from "@material-ui/core/IconButton";
import AddCircleOutline from "@material-ui/icons/AddCircleOutline";
import Typography from "@material-ui/core/Typography";

import { updateCheckbox } from "./redux/nodes";
import { setGraphData } from "./redux/graphData";
import { setNodeInfos } from "./redux/nodeInfo";
import { getGraphData } from "./redux/store";
import { setLinks } from "./redux/links";
import { setLinksTrans } from "./redux/linksTrans";

const OverflowTip = (props) => {
  const textElementRef = useRef(null);
  const [hoverStatus, setHover] = useState(false);

  const compareSize = (textElementRef) => {
    if (textElementRef.current != null) {
      const compare =
        textElementRef.current.scrollWidth > textElementRef.current.offsetWidth;
      setHover(compare);
    }
  };

  function newGraphData() {
    let gitHash = props.selectedGraph;
    if (gitHash) {
      let postData = {
          "selected_nodes": props.nodes.filter(node => node.selected == true).map(node => node.node),
          "transitive_edges": props.showTransitive
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
          props.setGraphData(data.graphData);
          props.setLinks(data.graphData.links);
          props.setLinksTrans(data.graphData.links_trans);
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
          props.setNodeInfos(data.nodeInfos);
        });
    }
  }

  useEffect(() => {
    compareSize(textElementRef);
    window.addEventListener("resize", compareSize);
    return function () {
      window.removeEventListener("resize", compareSize);
    };
  }, [props, textElementRef.current]);

  return (
    <Tooltip
      title={props.value}
      interactive="true"
      disableHoverListener={!hoverStatus}
      style={{ fontSize: "1em" }}
      enterDelay={500}
      TransitionComponent={Fade}
    >
      <Box
        style={{
          fontSize: "1em",
          whiteSpace: "nowrap",
          overflow: "hidden",
          textOverflow: "ellipsis",
        }}
      >
        <Typography noWrap variant={"body2"} gutterBottom>
          {props.button && (
            <IconButton
              size="small"
              color="secondary"
              onClick={(event) => {
                props.updateCheckbox({ node: props.text, value: "flip" });
                newGraphData();
              }}
            >
              <AddCircleOutline style={{ height: "15px", width: "15px" }} />
            </IconButton>
          )}
          <span ref={textElementRef}>{props.text}</span>
        </Typography>
      </Box>
    </Tooltip>
  );
};

export default connect(getGraphData, { updateCheckbox, setGraphData, setNodeInfos, setLinks, setLinksTrans })(OverflowTip);

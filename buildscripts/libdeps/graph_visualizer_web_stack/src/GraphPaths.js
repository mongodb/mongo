import React from "react";
import { connect } from "react-redux";
import { FixedSizeList } from "react-window";
import SplitPane from "react-split-pane";
import { makeStyles, withStyles } from "@material-ui/core/styles";
import ListItem from "@material-ui/core/ListItem";
import ListItemText from "@material-ui/core/ListItemText";
import Paper from "@material-ui/core/Paper";
import Typography from "@material-ui/core/Typography";
import Box from "@material-ui/core/Box";
import ExpandMoreIcon from "@material-ui/icons/ExpandMore";
import MuiAccordion from "@material-ui/core/Accordion";
import MuiAccordionSummary from "@material-ui/core/AccordionSummary";
import MuiAccordionDetails from "@material-ui/core/AccordionDetails";
import useResizeAware from "react-resize-aware";

import { getSelected } from "./redux/store";
import { selectedGraphPaths, setSelectedPath } from "./redux/graphPaths";
import { setGraphData } from "./redux/graphData";
import { setLinks } from "./redux/links";
import { setLinksTrans } from "./redux/linksTrans";

import OverflowTooltip from "./OverflowTooltip";

const rowHeight = 25;

const Accordion = withStyles({
  root: {
    border: "1px solid rgba(0, 0, 0, .125)",
    boxShadow: "none",
    "&:not(:last-child)": {
      borderBottom: 0,
    },
    "&:before": {
      display: "none",
    },
    "&$expanded": {
      margin: "auto",
    },
  },
  expanded: {},
})(MuiAccordion);

const AccordionSummary = withStyles({
  root: {
    backgroundColor: "rgba(0, 0, 0, .03)",
    borderBottom: "1px solid rgba(0, 0, 0, .125)",
    marginBottom: -1,
    minHeight: 56,
    "&$expanded": {
      minHeight: 56,
    },
  },
  content: {
    "&$expanded": {
      margin: "12px 0",
    },
  },
  expanded: {},
})(MuiAccordionSummary);

const AccordionDetails = withStyles((theme) => ({
  root: {
    padding: theme.spacing(2),
  },
}))(MuiAccordionDetails);

const GraphPaths = ({
  nodes,
  selectedGraph,
  selectedNodes,
  graphPaths,
  setSelectedPath,
  width,
  selectedGraphPaths,
  setGraphData,
  setLinks,
  setLinksTrans,
  showTransitive,
  transPathFrom,
  transPathTo
}) => {
  const [fromNode, setFromNode] = React.useState("");
  const [toNode, setToNode] = React.useState("");
  const [fromNodeId, setFromNodeId] = React.useState(0);
  const [toNodeId, setToNodeId] = React.useState(0);
  const [fromNodeExpanded, setFromNodeExpanded] = React.useState(false);
  const [toNodeExpanded, setToNodeExpanded] = React.useState(false);
  const [paneSize, setPaneSize] = React.useState("50%");

  const [fromResizeListener, fromSizes] = useResizeAware();
  const [toResizeListener, toSizes] = useResizeAware();

  const useStyles = makeStyles((theme) => ({
    root: {
      width: "100%",
      maxWidth: width,
      backgroundColor: theme.palette.background.paper,
    },
    nested: {
      paddingLeft: theme.spacing(4),
    },
    listItem: {
      width: width,
    },
  }));
  const classes = useStyles();
  
  React.useEffect(() => {
    setFromNode(transPathFrom);
    setFromNodeExpanded(false);
    setToNode(transPathFrom);
    setToNodeExpanded(false);
    setPaneSize("50%");
    if (transPathFrom != '' && transPathTo != '') {
      getGraphPaths(transPathFrom, transPathTo);
    } else {
      selectedGraphPaths({
        fromNode: '',
        toNode: '',
        paths: [],
        selectedPath: -1
      });
    }
  }, [transPathFrom, transPathTo]);

  function getGraphPaths(fromNode, toNode) {
    let gitHash = selectedGraph;
    if (gitHash) {
      let postData = {
        "fromNode": fromNode,
        "toNode": toNode
      };
      fetch('/api/graphs/' + gitHash + '/paths', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        },
        body: JSON.stringify(postData)
      })
        .then(response => response.json())
        .then(data => {
          selectedGraphPaths(data);
          let postData = {
            "selected_nodes": nodes.filter(node => node.selected == true).map(node => node.node),
            "extra_nodes": data.extraNodes,
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
                  setLinks(
                    data.graphData.links.map((link) => {
                      if (link.source == fromNode && link.target == toNode) {
                        link.selected = true;
                      } else {
                        link.selected = false;
                      }
                      return link;
                    })
                  );
                  setLinksTrans(data.graphData.links_trans);
              });
        });
      }
  }

  function toNodeRow({ index, style, data }) {
    return (
      <ListItem
        button
        style={style}
        key={index}
        onClick={() => {
          setToNode(data[index].name);
          setToNodeId(index);
          setToNodeExpanded(false);
          setPaneSize("50%");
          if (fromNode != "" && data[fromNodeId]) {
            getGraphPaths(data[fromNodeId].node, data[index].node);
          }
        }}
      >
        <ListItemText primary={data[index].name} />
      </ListItem>
    );
  }

  function fromNodeRow({ index, style, data }) {
    return (
      <ListItem
        button
        style={style}
        key={index}
        onClick={() => {
          setFromNode(data[index].name);
          setFromNodeId(index);
          setFromNodeExpanded(false);
          setPaneSize("50%");

          if (toNode != "" && data[toNodeId]) {
            getGraphPaths(data[fromNodeId].node, data[index].node);
          }
        }}
      >
        <ListItemText primary={data[index].name} />
      </ListItem>
    );
  }

  function pathRow({ index, style, data }) {
    return (
      <ListItem
        button
        style={style}
        key={index}
        onClick={() => {
          setSelectedPath(index);
        }}
      >
        <ListItemText
          primary={
            "Path #" +
            (index + 1).toString() +
            " - Hops: " +
            (data[index].length - 1).toString()
          }
        />
      </ListItem>
    );
  }

  function listHeight(numItems, minHeight, maxHeight) {
    const size = numItems * rowHeight;
    if (size > maxHeight) {
      return maxHeight;
    }
    if (size < minHeight) {
      return minHeight;
    }
    return size;
  }

  const handleToChange = (panel) => (event, newExpanded) => {
    setPaneSize(newExpanded ? "0%" : "50%");
    setToNodeExpanded(newExpanded ? panel : false);
  };

  const handleFromChange = (panel) => (event, newExpanded) => {
    setPaneSize(newExpanded ? "100%" : "50%");
    setFromNodeExpanded(newExpanded ? panel : false);
  };

  return (
    <Paper elevation={3} style={{ backgroundColor: "rgba(0, 0, 0, .03)" }}>
      <SplitPane
        split="vertical"
        minSize={"50%"}
        size={paneSize}
        style={{ position: "relative" }}
        defaultSize={"50%"}
        pane1Style={{ height: "100%" }}
        pane2Style={{ height: "100%", width: "100%" }}
      >
        <Accordion
          expanded={fromNodeExpanded}
          onChange={handleFromChange(!fromNodeExpanded)}
        >
          <AccordionSummary
            expandIcon={<ExpandMoreIcon />}
            aria-controls="panel1a-content"
            id="panel1a-header"
          >
            <Box
              style={{
                display: "flex",
                flexDirection: "column",
              }}
            >
              <Typography className={classes.heading}>From Node:</Typography>
              <Typography
                className={classes.heading}
                style={{ width: fromSizes.width - 50 }}
                noWrap={true}
                display={"block"}
              >
                {fromResizeListener}
                {fromNode}
              </Typography>
            </Box>
          </AccordionSummary>
          <AccordionDetails>
            <FixedSizeList
              height={listHeight(selectedNodes.length, 100, 200)}
              width={width}
              itemSize={rowHeight}
              itemCount={selectedNodes.length}
              itemData={selectedNodes}
            >
              {fromNodeRow}
            </FixedSizeList>
          </AccordionDetails>
        </Accordion>

        <Accordion
          expanded={toNodeExpanded}
          onChange={handleToChange(!toNodeExpanded)}
        >
          <AccordionSummary
            expandIcon={<ExpandMoreIcon />}
            aria-controls="panel1a-content"
            id="panel1a-header"
          >
            <Box style={{ display: "flex", flexDirection: "column" }}>
              <Typography className={classes.heading}>To Node:</Typography>
              <Typography
                className={classes.heading}
                style={{ width: toSizes.width - 50 }}
                noWrap={true}
                display={"block"}
              >
                {toResizeListener}
                {toNode}
              </Typography>
            </Box>
          </AccordionSummary>
          <AccordionDetails>
            <FixedSizeList
              height={listHeight(selectedNodes.length, 100, 200)}
              width={width}
              itemSize={rowHeight}
              itemCount={selectedNodes.length}
              itemData={selectedNodes}
            >
              {toNodeRow}
            </FixedSizeList>
          </AccordionDetails>
        </Accordion>
      </SplitPane>
      <Paper elevation={2} style={{ backgroundColor: "rgba(0, 0, 0, .03)" }}>
        <Typography className={classes.heading} style={{ margin: "10px" }}>
          Num Paths: {graphPaths.paths.length}{" "}
        </Typography>
      </Paper>
      <FixedSizeList
        height={listHeight(graphPaths.paths.length, 100, 200)}
        width={width}
        itemSize={rowHeight}
        itemCount={graphPaths.paths.length}
        itemData={graphPaths.paths}
        style={{ margin: "10px" }}
      >
        {pathRow}
      </FixedSizeList>
    </Paper>
  );
};

export default connect(getSelected, { selectedGraphPaths, setSelectedPath, setGraphData, setLinks, setLinksTrans })(
  GraphPaths
);

import React from "react";
import { makeStyles } from "@material-ui/core/styles";
import AppBar from "@material-ui/core/AppBar";
import Tabs from "@material-ui/core/Tabs";
import Tab from "@material-ui/core/Tab";

import NodeList from "./NodeList";
import EdgeList from "./EdgeList";
import InfoExpander from "./InfoExpander";
import AlgorithmExpander from "./AlgorithmExpander";

function a11yProps(index) {
  return {
    id: `scrollable-auto-tab-${index}`,
    "aria-controls": `scrollable-auto-tabpanel-${index}`,
  };
}

const useStyles = makeStyles((theme) => ({
  root: {
    flexGrow: 1,
    width: "100%",
    height: "100%",
    backgroundColor: theme.palette.background.paper,
  },
}));

export default function GraphInfoTabs({ nodes, width }) {
  const classes = useStyles();
  const [tab, setTab] = React.useState(1);
  const [transPathFrom, setTransPathFrom] = React.useState('');
  const [transPathTo, setTransPathTo] = React.useState('');

  const handleChange = (event, newValue) => {
    setTab(newValue);
  };

  const handleTransPath = (event, fromNode, toNode) => {
    setTransPathFrom(fromNode);
    setTransPathTo(toNode);
    if (fromNode != '' && toNode != '') {
      setTab(3);
    }
  };

  return (
    <div className={classes.root}>
      <AppBar position="static" color="default">
        <Tabs
          value={tab}
          onChange={handleChange}
          indicatorColor="primary"
          textColor="primary"
          variant="scrollable"
          scrollButtons="auto"
          aria-label="scrollable auto tabs example"
        >
          <Tab label="Selected Info" {...a11yProps(0)} />
          <Tab label="Node List" {...a11yProps(1)} />
          <Tab label="Edge List" {...a11yProps(2)} />
          <Tab label="Algorithms" {...a11yProps(3)} />
        </Tabs>
      </AppBar>
      <div style={{ height: "100%" }} hidden={tab != 0}>
        <InfoExpander width={width}></InfoExpander>
      </div>
      <div style={{ height: "100%" }} hidden={tab != 1}>
        <NodeList nodes={nodes}></NodeList>
      </div>
      <div style={{ height: "100%" }} hidden={tab != 2}>
        <EdgeList nodes={nodes} setTransPath={handleTransPath}></EdgeList>
      </div>
      <div style={{ height: "100%" }} hidden={tab != 3}>
        <AlgorithmExpander width={width} transPathFrom={transPathFrom} transPathTo={transPathTo}></AlgorithmExpander>
      </div>
    </div>
  );
}

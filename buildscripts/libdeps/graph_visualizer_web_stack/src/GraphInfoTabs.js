import React from "react";
import { makeStyles } from "@material-ui/core/styles";
import AppBar from "@material-ui/core/AppBar";
import Tabs from "@material-ui/core/Tabs";
import Tab from "@material-ui/core/Tab";

import NodeList from "./NodeList";
import InfoExpander from "./InfoExpander";

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
  const [value, setValue] = React.useState(0);

  const handleChange = (event, newValue) => {
    setValue(newValue);
  };

  return (
    <div className={classes.root}>
      <AppBar position="static" color="default">
        <Tabs
          value={value}
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
        </Tabs>
      </AppBar>
      <div style={{ height: "100%" }} hidden={value != 0}>
        <InfoExpander width={width}></InfoExpander>
      </div>
      <div style={{ height: "100%" }} hidden={value != 1}>
        <NodeList nodes={nodes}></NodeList>
      </div>
    </div>
  );
}

import React from "react";
import SplitPane from "react-split-pane";

import theme from "./theme";

import GraphCommitDisplay from "./GraphCommitDisplay";
import GraphInfoTabs from "./GraphInfoTabs";
import DrawGraph from "./DrawGraph";

const resizerStyle = {
  background: theme.palette.text.secondary,
  width: "1px",
  cursor: "col-resize",
  margin: "1px",
  padding: "1px",
  height: "100%",
};

const topPaneStyle = {
  height: "100vh",
  overflow: "visible",
};

export default function App() {
  const [infosize, setInfosize] = React.useState(450);
  const [drawsize, setDrawsize] = React.useState(
    window.screen.width - infosize
  );

  React.useEffect(() => {
    setInfosize(window.screen.width - drawsize);
  }, [drawsize]);

  return (
    <SplitPane
      pane1Style={{ height: "12%" }}
      pane2Style={{ height: "88%" }}
      split="horizontal"
      style={topPaneStyle}
    >
      <GraphCommitDisplay />
      <SplitPane
        split="vertical"
        minSize={100}
        style={{ position: "relative" }}
        defaultSize={infosize}
        pane1Style={{ height: "100%" }}
        pane2Style={{ height: "100%", width: "100%" }}
        resizerStyle={resizerStyle}
        onChange={(size) => setDrawsize(window.screen.width - size)}
      >
        <GraphInfoTabs width={infosize} />
        <DrawGraph size={drawsize} />
      </SplitPane>
    </SplitPane>
  );
}

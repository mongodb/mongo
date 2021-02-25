import React from "react";
import LinearProgress from "@material-ui/core/LinearProgress";
import Fade from "@material-ui/core/Fade";

export default function LoadingBar({ loading, height, children }) {
  const dimOnTrue = (flag) => {
    return {
      opacity: flag ? 0.15 : 1,
      height: "100%",
    };
  };

  return (
    <div style={{ height: height }}>
      <Fade
        in={loading}
        style={{ transitionDelay: loading ? "300ms" : "0ms" }}
        unmountOnExit
      >
        <LinearProgress />
      </Fade>
      <div style={dimOnTrue(loading)}>{children}</div>
    </div>
  );
}

import React, { useRef, useEffect, useState } from "react";
import { connect } from "react-redux";
import Tooltip from "@material-ui/core/Tooltip";
import Fade from "@material-ui/core/Fade";
import Box from "@material-ui/core/Box";
import IconButton from "@material-ui/core/IconButton";
import AddCircleOutline from "@material-ui/icons/AddCircleOutline";
import Typography from "@material-ui/core/Typography";

import { socket } from "./connect";
import { updateCheckbox } from "./redux/nodes";

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
      interactive
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
                socket.emit("row_selected", {
                  data: { node: props.text, name: props.name },
                  isSelected: "flip",
                });
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

export default connect(null, { updateCheckbox })(OverflowTip);

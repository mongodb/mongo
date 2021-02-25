import React from "react";
import { connect } from "react-redux";
import { makeStyles, withStyles } from "@material-ui/core/styles";
import Typography from "@material-ui/core/Typography";
import ExpandMoreIcon from "@material-ui/icons/ExpandMore";
import Paper from "@material-ui/core/Paper";
import MuiAccordion from "@material-ui/core/Accordion";
import MuiAccordionSummary from "@material-ui/core/AccordionSummary";
import MuiAccordionDetails from "@material-ui/core/AccordionDetails";

import { getSelected } from "./redux/store";

import GraphInfo from "./GraphInfo";
import NodeInfo from "./NodeInfo";
import LoadingBar from "./LoadingBar";

const useStyles = makeStyles((theme) => ({
  root: {
    width: "100%",
  },
  heading: {
    fontSize: theme.typography.pxToRem(15),
    fontWeight: theme.typography.fontWeightRegular,
  },
}));

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

const InfoExpander = ({ selectedNodes, selectedEdges, loading, width }) => {
  const classes = useStyles();

  return (
    <div className={classes.root}>
      <LoadingBar loading={loading} height={"100%"}>
        <Paper style={{ maxHeight: "82vh", overflow: "auto" }}>
          <Accordion>
            <AccordionSummary
              expandIcon={<ExpandMoreIcon />}
              aria-controls="panel1a-content"
              id="panel1a-header"
            >
              <Typography className={classes.heading}>Counts</Typography>
            </AccordionSummary>
            <AccordionDetails>
              <GraphInfo datawidth={width} />
            </AccordionDetails>
          </Accordion>
          {selectedNodes.map((node) => (
            <Accordion key={node.node}>
              <AccordionSummary
                expandIcon={<ExpandMoreIcon />}
                aria-controls="panel1a-content"
                id="panel1a-header"
              >
                <Typography className={classes.heading}>{node.name}</Typography>
              </AccordionSummary>
              <AccordionDetails>
                <NodeInfo node={node} width={width} />
              </AccordionDetails>
            </Accordion>
          ))}
        </Paper>
      </LoadingBar>
    </div>
  );
};

export default connect(getSelected)(InfoExpander);

import React, { useState } from "react";
import clsx from "clsx";
import { connect } from "react-redux";
import { getEdges } from "./redux/store";
import { setFindNode } from "./redux/findNode";
import { setLinks } from "./redux/links";
import { setGraphData } from "./redux/graphData";
import { setSelectedPath } from "./redux/graphPaths";
import { AutoSizer, Column, Table } from "react-virtualized";
import TableCell from "@material-ui/core/TableCell";
import Typography from "@material-ui/core/Typography";
import Tooltip from '@material-ui/core/Tooltip';
import GraphPaths from "./GraphPaths";

import { makeStyles, withStyles } from "@material-ui/core/styles";

import LoadingBar from "./LoadingBar";
import TextField from "@material-ui/core/TextField";
import { List, ListItemText, Paper, Button } from "@material-ui/core";

const columns = [
  { dataKey: "type", label: "Type", width: 30 },
  { dataKey: "source", label: "From", width: 180 },
  { dataKey: "to", label: "➔", width: 40 },
  { dataKey: "target", label: "To", width: 180 },
];

const visibilityTypes = ['Global', 'Public', 'Private', 'Interface'];

function componentToHex(c) {
    var hex = c.toString(16);
    return hex.length == 1 ? "0" + hex : hex;
  }
  
  function rgbToHex(r, g, b) {
    return "#" + componentToHex(r) + componentToHex(g) + componentToHex(b);
  }
  
  function hexToRgb(hex) {
    // Expand shorthand form (e.g. "03F") to full form (e.g. "0033FF")
    var shorthandRegex = /^#?([a-f\d])([a-f\d])([a-f\d])$/i;
    hex = hex.replace(shorthandRegex, function (m, r, g, b) {
      return r + r + g + g + b + b;
    });
  
    var result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
    return result
      ? {
          r: parseInt(result[1], 16),
          g: parseInt(result[2], 16),
          b: parseInt(result[3], 16),
        }
      : null;
  }
  
  function incrementPallete(palleteColor, increment) {
    var rgb = hexToRgb(palleteColor);
    rgb.r += increment;
    rgb.g += increment;
    rgb.b += increment;
    return rgbToHex(rgb.r, rgb.g, rgb.b);
  }
  
  const styles = (theme) => ({
    flexContainer: {
      display: "flex",
      alignItems: "center",
    },
    table: {
      // temporary right-to-left patch, waiting for
      // https://github.com/bvaughn/react-virtualized/issues/454
      "& .ReactVirtualized__Table__headerRow": {
        flip: false,
        paddingRight: theme.direction === "rtl" ? "0 !important" : undefined,
      },
    },
    tableRowOdd: {
      backgroundColor: incrementPallete(theme.palette.grey[800], 10),
    },
    tableRowEven: {
      backgroundColor: theme.palette.grey[800],
    },
    tableRowHover: {
      "&:hover": {
        backgroundColor: theme.palette.grey[600],
      },
    },
    tableCell: {
      flex: 1,
    },
    noClick: {
      cursor: "initial",
    },
  });

const EdgeList = ({ selectedGraph, links, setLinks, linksTrans, loading, setFindNode, classes, setTransPath }) => {
  const [searchTerm, setSearchTerm] = useState('');

  const selectedLinks = links.filter(link => link.selected);

  function searchedLinks() {
    if (searchTerm == '') {
      return links;
    }
    return links.filter(link => {
        if (link.source.name && link.target.name) {
            return link.source.name.indexOf(searchTerm) > -1 || link.target.name.indexOf(searchTerm) > -1;
        }});
  }

  function handleRowClick(event) {
    setLinks(
      links.map((temp_link, index) => {
        if (index == searchedLinks()[event.index].index) {
          temp_link.selected = !temp_link.selected;
        } else {
          temp_link.selected = false;
        }
        return temp_link;
      })
    );
    setTransPath(event, '', '');
  }

  function handleSearchTermChange(event) {
    setSearchTerm(event.target.value);
  }

  function reduceNodeName(node) {
    if (node.name) {
      return node.name;
    }
    return node.substring(node.lastIndexOf('/') + 1);
  }

  const getRowClassName = ({ index }) => {
    return clsx(
      index % 2 == 0 ? styles.tableRowEven : classes.tableRowOdd,
      classes.flexContainer,
      {
        [classes.tableRowHover]: index !== -1,
      }
    );
  };

  const cellRenderer = ({ cellData, columnIndex, rowIndex }) => {

    return (
      <TableCell
      component="div"
      >
        { columnIndex == 0 ?
            ( searchedLinks()[rowIndex].data?.direct ?
              <Tooltip title="DIRECT" placement="right" arrow><p>D</p></Tooltip>
              : 
              <Tooltip title="TRANSITIVE" placement="right" arrow><p>T</p></Tooltip>
            )
            :
            ""
        }
        { columnIndex == 1 ? reduceNodeName(searchedLinks()[rowIndex].source) : "" }
        { columnIndex == 2 ? (searchedLinks()[rowIndex].selected ? <span style={{ color: "#ED7811" }}>➔</span> : "➔") : "" }
        { columnIndex == 3 ? reduceNodeName(searchedLinks()[rowIndex].target) : "" }
      </TableCell>
    );
  };

  const headerRenderer = ({ label, columnIndex }) => {
    return (
      <TableCell
      component="div"
      >
        <Typography
          style={{ width: "100%" }}
          align="left"
          variant="caption"
          component="h2"
        >
            {label}
        </Typography>
      </TableCell>
    );
  };

  return (
    <LoadingBar loading={loading} height={"95%"}>
      <TextField
        fullWidth
        onChange={handleSearchTermChange}
        onClick={(event)=> event.target.select()}
        label="Search for Edge"
      />
      <div style={{ height: "30%" }}>
      <AutoSizer>
      {({ height, width }) => (
        <Table
          height={height}
          width={width}
          rowCount={searchedLinks().length}
          rowGetter={({ index }) => searchedLinks()[index].target}
          rowHeight={25}
          onRowClick={handleRowClick}
          gridStyle={{
            direction: "inherit",
          }}
          size={"small"}
          rowClassName={getRowClassName}
          headerHeight={35}
        >
          {columns.map(({ dataKey, ...other }, index) => {
            return (
              <Column
                key={dataKey}
                headerRenderer={(headerProps) =>
                  headerRenderer({
                    ...headerProps,
                    columnIndex: index,
                  })
                }
                cellRenderer={cellRenderer}
                dataKey={dataKey}
                {...other}
              />
            );
          })}
        </Table>
      )} 
    </AutoSizer>
    </div>
    <Paper style={{ border: "2px solid", height: "55%", padding: 5, overflow: 'auto' }} hidden={(selectedLinks.length <= 0)}>
      <List dense={true} style={{ padding: 5 }}>
        <Paper elevation={3} style={{ backgroundColor: "rgba(33, 33, 33)", padding: 15 }}>
          <h4 style={{ margin: 0 }}>{ selectedLinks[0]?.source.name } ➔ { selectedLinks[0]?.target.name }</h4>
          <ListItemText primary={ <span><strong>Type:</strong> { selectedLinks[0]?.data.direct ? "Direct" : "Transitive" }</span> }/>
          <ListItemText primary={ <span><strong>Visibility:</strong> { visibilityTypes[selectedLinks[0]?.data.visibility] }</span> }/>
          <ListItemText primary={ <span><strong>Source:</strong> { selectedLinks[0]?.source.id }</span> } secondary= { selectedLinks[0]?.source.type }/>
          <ListItemText primary={ <span><strong>Target:</strong> { selectedLinks[0]?.target.id }</span> } secondary= { selectedLinks[0]?.target.type }/>
          <div>
            <ListItemText primary={ <strong>Symbol Dependencies: { selectedLinks[0]?.data.symbols.length }</strong> }/>
            { selectedLinks[0]?.data.symbols.map((symbol, index) => {
                return (
                  <span key={index}>
                    <ListItemText secondary={ symbol } style={{textIndent: "-1em", marginLeft: "1em", overflowWrap: "break-word"}}></ListItemText>
                    <hr style={{ border: "0px", borderTop: "0.5px solid rgba(255, 255, 255, .2)", marginTop: "2px", marginBottom: "2px"}}></hr>
                  </span>
                );
              })
            }
          </div>
          <div hidden={(selectedLinks[0]?.data.direct ? "Direct" : "Transitive") == "Direct" }>
            <br></br>
            <Button variant="contained" onClick={ (event) => setTransPath(event, selectedLinks[0]?.source.id, selectedLinks[0]?.target.id) }>View Paths</Button>
          </div>
        </Paper>
      </List>
    </Paper>
    </LoadingBar>
  );
};

export default connect(getEdges, { setGraphData, setFindNode, setLinks, setSelectedPath })(withStyles(styles)(EdgeList));

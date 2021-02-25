import React from "react";
import { connect } from "react-redux";
import clsx from "clsx";
import { AutoSizer, Column, Table } from "react-virtualized";
import "react-virtualized/styles.css"; // only needs to be imported once
import { withStyles } from "@material-ui/core/styles";
import TableCell from "@material-ui/core/TableCell";
import { Checkbox } from "@material-ui/core";
import Typography from "@material-ui/core/Typography";

import { getRows } from "./redux/store";
import { updateSelected } from "./redux/nodes";
import { socket } from "./connect";

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
    backgroundColor: "#4d4d4d",
  },
  tableRowEven: {},
  tableRowHover: {
    "&:hover": {
      backgroundColor: theme.palette.grey[700],
    },
  },
  tableCell: {
    flex: 1,
  },
  noClick: {
    cursor: "initial",
  },
});

const DataGrid = ({
  rowGetter,
  rowCount,
  nodes,
  rowHeight,
  headerHeight,
  columns,
  onNodeClicked,
  updateSelected,
  classes,
}) => {
  const [checkBoxes, setCheckBoxes] = React.useState([]);

  React.useEffect(() => {
    setCheckBoxes(nodes);
  }, [nodes]);

  const getRowClassName = ({ index }) => {
    return clsx(
      index % 2 == 0 ? classes.tableRowEven : classes.tableRowOdd,
      classes.flexContainer,
      {
        [classes.tableRowHover]: index !== -1,
      }
    );
  };

  const cellRenderer = ({ cellData, columnIndex, rowIndex }) => {
    var finalCellData;
    var style = { height: rowHeight, padding: "0px" };
    if (cellData == "checkbox") {
      style["justifyContent"] = "space-evenly";
      finalCellData = (
        <Checkbox
          checked={checkBoxes[rowIndex].selected}
          onChange={(event) => {
            setCheckBoxes(
              checkBoxes.map((checkbox, index) => {
                if (index == rowIndex) {
                  checkbox.selected = event.target.checked;
                }
                return checkbox;
              })
            );
            if (checkBoxes[rowIndex].selected != event.target.checked) {
              updateSelected({ index: rowIndex, value: event.target.checked });
            }
            socket.emit("row_selected", {
              data: { node: nodes[rowIndex].node, name: nodes[rowIndex].name },
              isSelected: event.target.checked,
            });
          }}
        />
      );
    } else {
      finalCellData = cellData;
    }

    return (
      <TableCell
        component="div"
        className={clsx(
          classes.tableCell,
          classes.flexContainer,
          classes.noClick
        )}
        variant="body"
        onClick={onNodeClicked}
        style={style}
      >
        {finalCellData}
      </TableCell>
    );
  };

  const headerRenderer = ({ label, columnIndex }) => {
    return (
      <TableCell
        component="div"
        className={clsx(
          classes.tableCell,
          classes.flexContainer,
          classes.noClick
        )}
        variant="head"
        style={{ height: headerHeight, padding: "0px" }}
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
    <AutoSizer>
      {({ height, width }) => (
        <Table
          height={height}
          width={width}
          rowCount={rowCount}
          rowHeight={rowHeight}
          gridStyle={{
            direction: "inherit",
          }}
          size={"small"}
          rowGetter={rowGetter}
          className={clsx(classes.table, classes.noClick)}
          rowClassName={getRowClassName}
          headerHeight={headerHeight}
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
                className={classes.flexContainer}
                cellRenderer={cellRenderer}
                dataKey={dataKey}
                {...other}
              />
            );
          })}
        </Table>
      )}
    </AutoSizer>
  );
};

export default connect(getRows, { updateSelected })(
  withStyles(styles)(DataGrid)
);

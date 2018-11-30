// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package text provides utilities for formatting text data.
package text

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"strings"
)

type Cell struct {
	contents string
	feed     bool
}

type GridWriter struct {
	ColumnPadding int
	MinWidth      int
	Grid          [][]Cell
	CurrentRow    int
	colWidths     []int
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// init() makes the initial row if this is the first time any data is being written.
// otherwise, no-op.
func (gw *GridWriter) init() {
	if len(gw.Grid) <= gw.CurrentRow {
		gw.Grid = append(gw.Grid, []Cell{})
	}
}

// WriteCell writes the given string into the next cell in the current row.
func (gw *GridWriter) WriteCell(data string) {
	gw.init()
	gw.Grid[gw.CurrentRow] = append(gw.Grid[gw.CurrentRow], Cell{data, false})
}

// WriteCells writes multiple cells by calling WriteCell for each argument.
func (gw *GridWriter) WriteCells(data ...string) {
	for _, s := range data {
		gw.WriteCell(s)
	}
}

// Feed writes the given string into the current cell but allowing the cell contents
// to extend past the width of the current column, and ends the row.
func (gw *GridWriter) Feed(data string) {
	gw.init()
	gw.Grid[gw.CurrentRow] = append(gw.Grid[gw.CurrentRow], Cell{data, true})
	gw.EndRow()
}

// EndRow terminates the row of cells and begins a new row in the grid.
func (gw *GridWriter) EndRow() {
	gw.CurrentRow++
	if len(gw.Grid) <= gw.CurrentRow {
		gw.Grid = append(gw.Grid, []Cell{})
	}
}

// Reset discards any grid data and resets the current row.
func (gw *GridWriter) Reset() {
	gw.CurrentRow = 0
	gw.Grid = [][]Cell{}
}

// updateWidths sets the column widths in the Grid. For each column in the Grid,
// it updates the cached width if its value is less than the current width.
func (gw *GridWriter) updateWidths(colWidths []int) {
	if gw.colWidths == nil {
		gw.colWidths = make([]int, len(colWidths))
		copy(gw.colWidths, colWidths)
	}
	for i, cw := range colWidths {
		if gw.colWidths[i] < cw {
			gw.colWidths[i] = cw
		}
	}
}

// calculateWidths returns an array containing the correct padded size for
// each column in the grid.
func (gw *GridWriter) calculateWidths() []int {
	colWidths := []int{}

	// Loop over each column
	for j := 0; ; j++ {
		found := false

		// Examine all the rows at column 'j'
		for i := range gw.Grid {
			if len(gw.Grid[i]) <= j {
				continue
			}
			found = true

			if len(colWidths) <= j {
				colWidths = append(colWidths, 0)
			}

			if gw.Grid[i][j].feed {
				// we're at a row-terminating cell - skip over the rest of this row
				continue
			}
			// Set the size for the row to be the largest
			// of all the cells in the column
			newMin := max(gw.MinWidth, len(gw.Grid[i][j].contents))
			if newMin > colWidths[j] {
				colWidths[j] = newMin
			}
		}
		// This column did not have any data in it at all, so we've hit the
		// end of the grid - stop.
		if !found {
			break
		}
	}
	return colWidths
}

// Flush writes the fully-formatted grid to the given io.Writer.
func (gw *GridWriter) Flush(w io.Writer) {
	colWidths := gw.calculateWidths()

	// invalidate all cached widths if new cells are added/removed
	if len(gw.colWidths) != len(colWidths) {
		gw.colWidths = make([]int, len(colWidths))
		copy(gw.colWidths, colWidths)
	} else {
		gw.updateWidths(colWidths)
	}

	for i, row := range gw.Grid {
		lastRow := i == (len(gw.Grid) - 1)
		for j, cell := range row {
			lastCol := (j == len(row)-1)
			fmt.Fprintf(w, fmt.Sprintf("%%%vs", gw.colWidths[j]), cell.contents)
			if gw.ColumnPadding > 0 && !lastCol {
				fmt.Fprint(w, strings.Repeat(" ", gw.ColumnPadding))
			}
		}
		if !lastRow {
			fmt.Fprint(w, "\n")
		}
	}
}

// FlushRows writes the fully-formatted grid to the given io.Writer, but
// gives each row its own Write() call instead of using newlines.
func (gw *GridWriter) FlushRows(w io.Writer) {
	gridBuff := &bytes.Buffer{}
	gw.Flush(gridBuff)
	lineScanner := bufio.NewScanner(gridBuff)
	for lineScanner.Scan() {
		w.Write(lineScanner.Bytes())
	}
}

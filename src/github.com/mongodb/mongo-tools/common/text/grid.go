package text

import (
	"fmt"
	"io"
	"strings"
)

type GridWriter struct {
	ColumnPadding int
	MinWidth      int
	Grid          [][]string
	CurrentRow    int
	CurrentCol    int
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

//WriteCell writes the given string into the next cell in the current row.
func (gw *GridWriter) WriteCell(data string) {
	//If this is the first time being used, make the initial row
	if len(gw.Grid) <= gw.CurrentRow {
		gw.Grid = append(gw.Grid, []string{})
	}
	gw.Grid[gw.CurrentRow] = append(gw.Grid[gw.CurrentRow], data)
}

//EndRow terminates the row of cells and begins a new row in the grid.
func (gw *GridWriter) EndRow() {
	gw.CurrentRow += 1
	if len(gw.Grid) <= gw.CurrentRow {
		gw.Grid = append(gw.Grid, []string{})
	}
}

//calculateWidths returns an array containing the correct padded size for
//each column in the grid.
func (gw *GridWriter) calculateWidths() []int {
	colWidths := []int{}
	//Loop over each column
	for j := 0; ; j++ {
		found := false

		//Examine all the rows at column 'j'
		for i, _ := range gw.Grid {
			if len(gw.Grid[i]) <= j {
				continue
			}
			found = true

			if len(colWidths) <= j {
				colWidths = append(colWidths, 0)
			}
			//Set the size for the row to be the largest
			//of all the cells in the column
			newMin := max(gw.MinWidth, len(gw.Grid[i][j]))
			if newMin > colWidths[j] {
				colWidths[j] = newMin
			}
		}
		//This column did not have any data in it at all, so we've hit the
		//end of the grid - stop.
		if !found {
			break
		}
	}
	return colWidths
}

//Flush writes the fully-formatted grid to the given io.Writer.
func (gw *GridWriter) Flush(w io.Writer) {
	colWidths := gw.calculateWidths()
	for i, row := range gw.Grid {
		lastRow := i == (len(gw.Grid) - 1)
		for j, cell := range row {
			lastCol := (j == len(row)-1)
			width := colWidths[j]
			fmt.Fprintf(w, fmt.Sprintf("%%%vs", width), cell)
			if gw.ColumnPadding > 0 && !lastCol {
				fmt.Fprint(w, strings.Repeat(" ", gw.ColumnPadding))
			}
		}
		if !lastRow {
			fmt.Fprint(w, "\n")
		}
	}
}

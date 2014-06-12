// Package output implements means of outputting the results of mongotop's
// queries against MongoDB.
package output

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongotop/command"
	"strings"
)

// Interface to output the results of the top command.
type Outputter interface {
	Output(command.Diff) error
}

// Outputter that formats the results and prints them to the terminal.
type TerminalOutputter struct {
}

func (self *TerminalOutputter) Output(diff command.Diff) error {

	tableRows := diff.ToRows()

	// get the length of the longest row (the one with the most fields)
	longestRow := 0
	for _, row := range tableRows {
		longestRow = util.MaxInt(longestRow, len(row))
	}

	// bookkeep the length of the longest member of each column
	longestFields := make([]int, longestRow)
	for _, row := range tableRows {
		for idx, field := range row {
			longestFields[idx] = util.MaxInt(longestFields[idx], len(field))
		}
	}

	// write out each row
	for _, row := range tableRows {
		for idx, rowEl := range row {
			fmt.Printf("\t\t%v%v", strings.Repeat(" ",
				longestFields[idx]-len(rowEl)), rowEl)
		}
		fmt.Printf("\n")
	}
	fmt.Printf("\n")

	return nil

}

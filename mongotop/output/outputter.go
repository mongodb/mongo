// Package output implements means of outputting the results of mongotop's
// queries against MongoDB.
package output

import (
	"encoding/json"
	"fmt"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongotop/command"
	"strings"
)

// Interface to output the results of the top command.
type Outputter interface {
	Output(command.Diff) error
}

// Outputter that formats the results as json and prints them out.
type JSONOutputter struct{}

func (self *JSONOutputter) Output(diff command.Diff) error {

	rows := diff.ToRows()

	if len(rows) == 0 {
		return fmt.Errorf("no header row given")
	}

	// pull the diff rows into a json format
	jsonFormat := map[string]interface{}{}

	// pull out the header row
	headerRow := rows[0]
	if len(headerRow) == 0 {
		return fmt.Errorf("header row can't be blank")
	}

	// pull out the timestamp from the header row
	tsIndex := len(headerRow) - 1
	ts := headerRow[tsIndex]
	jsonFormat["ts"] = ts

	// pull the timestamp out of the header row
	headerRow = headerRow[:tsIndex]

	// loop over all rows after the header row, adding an entry for the
	// relevant namespace into the json
	for _, row := range rows[1:] {
		rowJson := map[string]string{}

		// skip the ns field
		for idx, header := range headerRow[1:] {
			rowJson[header] = row[idx+1]
		}

		// key into the main json by the ns field
		jsonFormat[row[0]] = rowJson
	}

	jsonFormatAsBytes, err := json.Marshal(jsonFormat)
	if err != nil {
		return fmt.Errorf("error converting output to json: %v", err)
	}

	fmt.Printf("%v\n", string(jsonFormatAsBytes))
	return nil

}

// Outputter that formats the results and prints them to the terminal.
type GridOutputter struct{}

func (self *GridOutputter) Output(diff command.Diff) error {

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

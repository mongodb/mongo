package output

import (
	"fmt"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/mongotop/result"
	"sort"
	"strconv"
	"strings"
	"time"
)

// output top results
type Outputter interface {
	Output(*result.TopResults, *commonopts.MongoToolOptions) error
}

// outputter that sends the results to the terminal
type TerminalOutputter struct {
}

// satisfy the Outputter interface
func (self *TerminalOutputter) Output(results *result.TopResults,
	opts *commonopts.MongoToolOptions) error {

	// bookkeep the longest member of each column, for spacing
	longestNS := len("ns")
	longestTotal := len("total")
	longestRead := len("read")
	longestWrite := len("write")

	// for sorting the namespaces
	namespaces := []string{}

	// figure out the longest of each column
	for ns, topInfo := range results.Totals {
		namespaces = append(namespaces, ns)

		// the output fields
		totalOutputField := timeOutputField(topInfo.Total.Time)
		readOutputField := timeOutputField(topInfo.Read.Time)
		writeOutputField := timeOutputField(topInfo.Write.Time)

		if len(ns) > longestNS {
			longestNS = len(ns)
		}
		if len(totalOutputField) > longestTotal {
			longestTotal = len(totalOutputField)
		}
		if len(readOutputField) > longestRead {
			longestRead = len(readOutputField)
		}
		if len(writeOutputField) > longestWrite {
			longestWrite = len(writeOutputField)
		}
	}

	// sort the namespaces
	sort.Strings(namespaces)

	// paddings for the header column
	namespacePadding := strings.Repeat(" ", longestNS-len("ns"))
	totalPadding := strings.Repeat(" ", longestTotal-len("total"))
	readPadding := strings.Repeat(" ", longestRead-len("read"))
	writePadding := strings.Repeat(" ", longestWrite-len("write"))

	// padding
	fmt.Println()

	// print the header column
	fmt.Println(
		fmt.Sprintf(
			"\t%vns\t\t%vtotal\t\t%vread\t\t%vwrite\t\t%v",
			namespacePadding,
			totalPadding,
			readPadding,
			writePadding,
			time.Now().Format(time.RFC3339),
		),
	)

	// print each namespace column
	for _, ns := range namespaces {
		if skipNamespace(ns, opts) {
			continue
		}
		topInfo := results.Totals[ns]

		// the output fields
		totalOutputField := timeOutputField(topInfo.Total.Time)
		readOutputField := timeOutputField(topInfo.Read.Time)
		writeOutputField := timeOutputField(topInfo.Write.Time)

		fmt.Println(
			fmt.Sprintf(
				"\t%v%v\t\t%v%v\t\t%v%v\t\t%v%v",
				strings.Repeat(" ", longestNS-len(ns)),
				ns,
				strings.Repeat(" ", longestTotal-len(totalOutputField)),
				totalOutputField,
				strings.Repeat(" ", longestRead-len(readOutputField)),
				readOutputField,
				strings.Repeat(" ", longestWrite-len(writeOutputField)),
				writeOutputField,
			),
		)
	}

	return nil
}

func timeOutputField(timeVal int) string {
	return strconv.Itoa(timeVal/1000) + "ms"
}

func skipNamespace(ns string, opts *commonopts.MongoToolOptions) bool {
	if opts.FilterNS != "" {
		if opts.FilterBoth {
			return ns != opts.FilterNS
		} else if opts.FilterOnlyColl {
			return !strings.HasSuffix(ns, opts.FilterNS)
		}
		return !strings.HasPrefix(ns, opts.FilterNS)
	}
	return ns == "" ||
		!strings.Contains(ns, ".") ||
		strings.HasSuffix(ns, "namespaces") ||
		strings.HasPrefix(ns, "local")
}

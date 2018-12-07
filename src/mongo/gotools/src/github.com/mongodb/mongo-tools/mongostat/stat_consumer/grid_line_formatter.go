package stat_consumer

import (
	"bytes"
	"fmt"
	"sort"
	"strings"

	"github.com/mongodb/mongo-tools/common/text"
	"github.com/mongodb/mongo-tools/mongostat/stat_consumer/line"
)

// GridLineFormatter uses a text.GridWriter to format the StatLines as a grid
type GridLineFormatter struct {
	*limitableFormatter
	*text.GridWriter

	// If true, enables printing of headers to output
	includeHeader bool

	// Counter for periodic headers
	index int

	// Tracks number of hosts so we can reprint headers when it changes
	prevLineCount int
}

func NewGridLineFormatter(maxRows int64, includeHeader bool) LineFormatter {
	return &GridLineFormatter{
		limitableFormatter: &limitableFormatter{maxRows: maxRows},
		includeHeader:      includeHeader,
		GridWriter:         &text.GridWriter{ColumnPadding: 1},
	}
}

func init() {
	FormatterConstructors[""] = NewGridLineFormatter
}

// headerInterval is the number of chunks before the header is re-printed in GridLineFormatter
const headerInterval = 10

func (glf *GridLineFormatter) Finish() {
}

// FormatLines formats the StatLines as a grid
func (glf *GridLineFormatter) FormatLines(lines []*line.StatLine, headerKeys []string, keyNames map[string]string) string {
	buf := &bytes.Buffer{}

	// Sort the stat lines by hostname, so that we see the output
	// in the same order for each snapshot
	sort.Sort(line.StatLines(lines))

	// Print the columns that are enabled
	for _, key := range headerKeys {
		header := keyNames[key]
		glf.WriteCell(header)
	}
	glf.EndRow()

	for _, l := range lines {
		if l.Printed && l.Error == nil {
			l.Error = fmt.Errorf("no data received")
		}
		l.Printed = true

		if l.Error != nil {
			glf.WriteCell(l.Fields["host"])
			glf.Feed(l.Error.Error())
			continue
		}

		for _, key := range headerKeys {
			glf.WriteCell(l.Fields[key])
		}
		glf.EndRow()
	}
	glf.Flush(buf)

	// clear the flushed data
	glf.Reset()

	gridLine := buf.String()

	if glf.prevLineCount != len(lines) {
		glf.index = 0
	}
	glf.prevLineCount = len(lines)

	if !glf.includeHeader || glf.index != 0 {
		// Strip out the first line of the formatted output,
		// which contains the headers. They've been left in up until this point
		// in order to force the formatting of the columns to be wide enough.
		firstNewLinePos := strings.Index(gridLine, "\n")
		if firstNewLinePos >= 0 {
			gridLine = gridLine[firstNewLinePos+1:]
		}
	}
	glf.index++
	if glf.index == headerInterval {
		glf.index = 0
	}

	if len(lines) > 1 {
		// For multi-node stats, add an extra newline to tell each block apart
		gridLine = fmt.Sprintf("\n%s", gridLine)
	}
	glf.increment()
	return gridLine
}

// +build !solaris

package stat_consumer

import (
	"fmt"
	"sort"
	"strings"

	"github.com/mongodb/mongo-tools/mongostat/stat_consumer/line"
	"github.com/nsf/termbox-go"
)

// InteractiveLineFormatter produces ncurses-style output
type InteractiveLineFormatter struct {
	*limitableFormatter

	includeHeader bool
	table         []*column
	row, col      int
	showHelp      bool
}

func NewInteractiveLineFormatter(_ int64, includeHeader bool) LineFormatter {
	ilf := &InteractiveLineFormatter{
		limitableFormatter: &limitableFormatter{maxRows: 1},
		includeHeader:      includeHeader,
	}
	if err := termbox.Init(); err != nil {
		fmt.Printf("Error setting up terminal UI: %v", err)
		panic("could not set up interactive terminal interface")
	}
	go func() {
		for {
			ilf.handleEvent(termbox.PollEvent())
			ilf.update()
		}
	}()
	return ilf
}

func init() {
	FormatterConstructors["interactive"] = NewInteractiveLineFormatter
}

type column struct {
	cells []*cell
	width int
}

type cell struct {
	text     string
	changed  bool
	feed     bool
	selected bool
	header   bool
}

// FormatLines formats the StatLines as a table in the terminal ui
func (ilf *InteractiveLineFormatter) FormatLines(lines []*line.StatLine, headerKeys []string, keyNames map[string]string) string {
	// keep ordering consistent
	sort.Sort(line.StatLines(lines))

	if ilf.includeHeader {
		headerLine := &line.StatLine{
			Fields: keyNames,
		}
		lines = append([]*line.StatLine{headerLine}, lines...)
	}

	// add new rows and columns when new hosts and stats are shown
	for len(ilf.table) < len(headerKeys) {
		ilf.table = append(ilf.table, new(column))
	}
	for _, column := range ilf.table {
		for len(column.cells) < len(lines) {
			column.cells = append(column.cells, new(cell))
		}
	}

	for i, column := range ilf.table {
		key := headerKeys[i]
		for j, cell := range column.cells {
			// i, j <=> col, row
			l := lines[j]
			if l.Error != nil && i == 0 {
				cell.text = fmt.Sprintf("%s: %s", l.Fields["host"], l.Error)
				cell.feed = true
				continue
			}
			newText := l.Fields[key]
			cell.changed = cell.text != newText
			cell.text = newText
			cell.feed = false
			cell.header = j == 0 && ilf.includeHeader
			if w := len(cell.text); w > column.width {
				column.width = w
			}
		}
	}

	ilf.update()
	return ""
}

func (ilf *InteractiveLineFormatter) handleEvent(ev termbox.Event) {
	if ev.Type != termbox.EventKey {
		return
	}
	currSelected := ilf.table[ilf.col].cells[ilf.row].selected
	switch {
	case ev.Key == termbox.KeyCtrlC:
		fallthrough
	case ev.Key == termbox.KeyEsc:
		fallthrough
	case ev.Ch == 'q':
		termbox.Close()
		// our max rowCount is set to 1; increment to exit
		ilf.increment()
	case ev.Key == termbox.KeyArrowRight:
		fallthrough
	case ev.Ch == 'l':
		if ilf.col+1 < len(ilf.table) {
			ilf.col++
		}
	case ev.Key == termbox.KeyArrowLeft:
		fallthrough
	case ev.Ch == 'h':
		if ilf.col > 0 {
			ilf.col--
		}
	case ev.Key == termbox.KeyArrowDown:
		fallthrough
	case ev.Ch == 'j':
		if ilf.row+1 < len(ilf.table[0].cells) {
			ilf.row++
		}
	case ev.Key == termbox.KeyArrowUp:
		fallthrough
	case ev.Ch == 'k':
		if ilf.row > 0 {
			ilf.row--
		}
	case ev.Ch == 's':
		cell := ilf.table[ilf.col].cells[ilf.row]
		cell.selected = !cell.selected
	case ev.Key == termbox.KeySpace:
		for _, column := range ilf.table {
			for _, cell := range column.cells {
				cell.selected = false
			}
		}
	case ev.Ch == 'c':
		for _, cell := range ilf.table[ilf.col].cells {
			cell.selected = !currSelected
		}
	case ev.Ch == 'v':
		for _, column := range ilf.table {
			cell := column.cells[ilf.row]
			cell.selected = !currSelected
		}
	case ev.Ch == 'r':
		termbox.Sync()
	case ev.Ch == '?':
		ilf.showHelp = !ilf.showHelp
	default:
		// ouput a bell on unknown inputs
		fmt.Printf("\a")
	}
}

const (
	helpPrompt  = `Press '?' to toggle help`
	helpMessage = `
Exit: 'q' or <Esc>
Navigation: arrow keys or 'h', 'j', 'k', and 'l'
Highlighting: 'v' to toggle row
              'c' to toggle column
              's' to toggle cell
              <Space> to clear all highlighting
Redraw: 'r' to fix broken-looking output`
)

func writeString(x, y int, text string, fg, bg termbox.Attribute) {
	for i, str := range strings.Split(text, "\n") {
		for j, ch := range str {
			termbox.SetCell(x+j, y+i, ch, fg, bg)
		}
	}
}

func (ilf *InteractiveLineFormatter) update() {
	termbox.Clear(termbox.ColorDefault, termbox.ColorDefault)
	x := 0
	for i, column := range ilf.table {
		for j, cell := range column.cells {
			if ilf.col == i && ilf.row == j {
				termbox.SetCursor(x+column.width-1, j)
			}
			if cell.text == "" {
				continue
			}
			fgAttr := termbox.ColorWhite
			bgAttr := termbox.ColorDefault
			if cell.selected {
				fgAttr = termbox.ColorBlack
				bgAttr = termbox.ColorWhite
			}
			if cell.changed || cell.feed {
				fgAttr |= termbox.AttrBold
			}
			if cell.header {
				fgAttr |= termbox.AttrUnderline
				fgAttr |= termbox.AttrBold
			}
			padding := column.width - len(cell.text)
			if cell.feed && padding < 0 {
				padding = 0
			}
			writeString(x, j, strings.Repeat(" ", padding), termbox.ColorDefault, bgAttr)
			writeString(x+padding, j, cell.text, fgAttr, bgAttr)
		}
		x += 1 + column.width
	}
	rowCount := len(ilf.table[0].cells)
	writeString(0, rowCount+1, helpPrompt, termbox.ColorWhite, termbox.ColorDefault)
	if ilf.showHelp {
		writeString(0, rowCount+2, helpMessage, termbox.ColorWhite, termbox.ColorDefault)
	}
	termbox.Flush()
}

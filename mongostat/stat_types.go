package mongostat

import (
	"bytes"
	"encoding/json"
	"fmt"
	"github.com/mongodb/mongo-tools/common/text"
	"github.com/mongodb/mongo-tools/common/util"
	"sort"
	"strings"
	"time"
)

const (
	MongosProcess = "mongos"
)

// Flags to determine cases when to activate/deactivate columns for output.
const (
	Always   = 1 << iota // always activate the column
	Discover             // only active when mongostat is in discover mode
	Repl                 // only active if one of the nodes being monitored is in a replset
	Locks                // only active if node is capable of calculating lock info
	AllOnly              // only active if mongostat was run with --all option
	MMAPOnly             // only active if node has mmap-specific fields
	WTOnly               // only active if node has wiredtiger-specific fields
)

type StatLines []StatLine

func (slice StatLines) Len() int {
	return len(slice)
}

func (slice StatLines) Less(i, j int) bool {
	if slice[i].Key == slice[j].Key {
		return slice[i].Host < slice[j].Host
	}
	return slice[i].Key < slice[j].Key
}

func (slice StatLines) Swap(i, j int) {
	slice[i], slice[j] = slice[j], slice[i]
}

type ServerStatus struct {
	SampleTime         time.Time              `bson:""`
	Host               string                 `bson:"host"`
	Version            string                 `bson:"version"`
	Process            string                 `bson:"process"`
	Pid                int64                  `bson:"pid"`
	Uptime             int64                  `bson:"uptime"`
	UptimeMillis       int64                  `bson:"uptimeMillis"`
	UptimeEstimate     int64                  `bson:"uptimeEstimate"`
	LocalTime          time.Time              `bson:"localTime"`
	Asserts            map[string]int64       `bson:"asserts"`
	BackgroundFlushing *FlushStats            `bson:"backgroundFlushing"`
	ExtraInfo          *ExtraInfo             `bson:"extra_info"`
	Connections        *ConnectionStats       `bson:"connections"`
	Dur                *DurStats              `bson:"dur"`
	GlobalLock         *GlobalLockStats       `bson:"globalLock"`
	Locks              map[string]LockStats   `bson:"locks,omitempty"`
	Network            *NetworkStats          `bson:"network"`
	Opcounters         *OpcountStats          `bson:"opcounters"`
	OpcountersRepl     *OpcountStats          `bson:"opcountersRepl"`
	RecordStats        *DBRecordStats         `bson:"recordStats"`
	Mem                *MemStats              `bson:"mem"`
	Repl               *ReplStatus            `bson:"repl"`
	ShardCursorType    map[string]interface{} `bson:"shardCursorType"`
	StorageEngine      map[string]string      `bson:"storageEngine"`
	WiredTiger         *WiredTiger            `bson:"wiredTiger"`
}

// WiredTiger stores information related to the WiredTiger storage engine.
type WiredTiger struct {
	Transaction TransactionStats       `bson:"transaction"`
	Concurrent  ConcurrentTransactions `bson:"concurrentTransactions"`
	Cache       CacheStats             `bson:"cache"`
}

type ConcurrentTransactions struct {
	Write ConcurrentTransStats `bson:"write"`
	Read  ConcurrentTransStats `bson:"read"`
}

type ConcurrentTransStats struct {
	Out int64 `bson:"out"`
}

// CacheStats stores cache statistics for WiredTiger.
type CacheStats struct {
	TrackedDirtyBytes  int64 `bson:"tracked dirty bytes in the cache"`
	CurrentCachedBytes int64 `bson:"bytes currently in the cache"`
	MaxBytesConfigured int64 `bson:"maximum bytes configured"`
}

// TransactionStats stores transaction checkpoints in WiredTiger.
type TransactionStats struct {
	TransCheckpoints int64 `bson:"transaction checkpoints"`
}

// ReplStatus stores data related to replica sets.
type ReplStatus struct {
	SetName      interface{} `bson:"setName"`
	IsMaster     interface{} `bson:"ismaster"`
	Secondary    interface{} `bson:"secondary"`
	IsReplicaSet interface{} `bson:"isreplicaset"`
	ArbiterOnly  interface{} `bson:"arbiterOnly"`
	Hosts        []string    `bson:"hosts"`
	Passives     []string    `bson:"passives"`
	Me           string      `bson:"me"`
}

// DBRecordStats stores data related to memory operations across databases.
type DBRecordStats struct {
	AccessesNotInMemory       int64                     `bson:"accessesNotInMemory"`
	PageFaultExceptionsThrown int64                     `bson:"pageFaultExceptionsThrown"`
	DBRecordAccesses          map[string]RecordAccesses `bson:",inline"`
}

// RecordAccesses stores data related to memory operations scoped to a database.
type RecordAccesses struct {
	AccessesNotInMemory       int64 `bson:"accessesNotInMemory"`
	PageFaultExceptionsThrown int64 `bson:"pageFaultExceptionsThrown"`
}

// MemStats stores data related to memory statistics.
type MemStats struct {
	Bits              int64       `bson:"bits"`
	Resident          int64       `bson:"resident"`
	Virtual           int64       `bson:"virtual"`
	Supported         interface{} `bson:"supported"`
	Mapped            int64       `bson:"mapped"`
	MappedWithJournal int64       `bson:"mappedWithJournal"`
}

// FlushStats stores information about memory flushes.
type FlushStats struct {
	Flushes      int64     `bson:"flushes"`
	TotalMs      int64     `bson:"total_ms"`
	AverageMs    float64   `bson:"average_ms"`
	LastMs       int64     `bson:"last_ms"`
	LastFinished time.Time `bson:"last_finished"`
}

// ConnectionStats stores information related to incoming database connections.
type ConnectionStats struct {
	Current      int64 `bson:"current"`
	Available    int64 `bson:"available"`
	TotalCreated int64 `bson:"totalCreated"`
}

// DurTiming stores information related to journaling.
type DurTiming struct {
	Dt               int64 `bson:"dt"`
	PrepLogBuffer    int64 `bson:"prepLogBuffer"`
	WriteToJournal   int64 `bson:"writeToJournal"`
	WriteToDataFiles int64 `bson:"writeToDataFiles"`
	RemapPrivateView int64 `bson:"remapPrivateView"`
}

// DurStats stores information related to journaling statistics.
type DurStats struct {
	Commits            int64 `bson:"commits"`
	JournaledMB        int64 `bson:"journaledMB"`
	WriteToDataFilesMB int64 `bson:"writeToDataFilesMB"`
	Compression        int64 `bson:"compression"`
	CommitsInWriteLock int64 `bson:"commitsInWriteLock"`
	EarlyCommits       int64 `bson:"earlyCommits"`
	TimeMs             DurTiming
}

// QueueStats stores the number of queued read/write operations.
type QueueStats struct {
	Total   int64 `bson:"total"`
	Readers int64 `bson:"readers"`
	Writers int64 `bson:"writers"`
}

// ClientStats stores the number of active read/write operations.
type ClientStats struct {
	Total   int64 `bson:"total"`
	Readers int64 `bson:"readers"`
	Writers int64 `bson:"writers"`
}

// GlobalLockStats stores information related locks in the MMAP storage engine.
type GlobalLockStats struct {
	TotalTime     int64        `bson:"totalTime"`
	LockTime      int64        `bson:"lockTime"`
	CurrentQueue  *QueueStats  `bson:"currentQueue"`
	ActiveClients *ClientStats `bson:"activeClients"`
}

// NetworkStats stores information related to network traffic.
type NetworkStats struct {
	BytesIn     int64 `bson:"bytesIn"`
	BytesOut    int64 `bson:"bytesOut"`
	NumRequests int64 `bson:"numRequests"`
}

// OpcountStats stores information related to comamnds and basic CRUD operations.
type OpcountStats struct {
	Insert  int64 `bson:"insert"`
	Query   int64 `bson:"query"`
	Update  int64 `bson:"update"`
	Delete  int64 `bson:"delete"`
	GetMore int64 `bson:"getmore"`
	Command int64 `bson:"command"`
}

// ReadWriteLockTimes stores time spent holding read/write locks.
type ReadWriteLockTimes struct {
	Read       int64 `bson:"R"`
	Write      int64 `bson:"W"`
	ReadLower  int64 `bson:"r"`
	WriteLower int64 `bson:"w"`
}

// LockStats stores information related to time spent acquiring/holding locks
// for a given database.
type LockStats struct {
	TimeLockedMicros    ReadWriteLockTimes `bson:"timeLockedMicros"`
	TimeAcquiringMicros ReadWriteLockTimes `bson:"timeAcquiringMicros"`

	// AcquireCount and AcquireWaitCount are new fields of the lock stats only populated on 3.0 or newer.
	// Typed as a pointer so that if it is nil, mongostat can assume the field is not populated
	// with real namespace data.
	AcquireCount     *ReadWriteLockTimes `bson:"acquireCount,omitempty"`
	AcquireWaitCount *ReadWriteLockTimes `bson:"acquireWaitCount,omitempty"`
}

// ExtraInfo stores additional platform specific information.
type ExtraInfo struct {
	PageFaults *int64 `bson:"page_faults"`
}

// StatHeader describes a single column for mongostat's terminal output,
// its formatting, and in which modes it should be displayed.
type StatHeader struct {
	// The text to appear in the column's header cell
	HeaderText string

	// Bitmask containing flags to determine if this header is active or not
	ActivateFlags int
}

// StatHeaders are the complete set of data metrics supported by mongostat.
var StatHeaders = []StatHeader{
	{"", Always}, // placeholder for hostname column (blank header text)
	{"insert", Always},
	{"query", Always},
	{"update", Always},
	{"delete", Always},
	{"getmore", Always},
	{"command", Always},
	{"% dirty", WTOnly},
	{"% used", WTOnly},
	{"flushes", Always},
	{"mapped", MMAPOnly},
	{"vsize", Always},
	{"res", Always},
	{"non-mapped", MMAPOnly | AllOnly},
	{"faults", MMAPOnly},
	{"lr|lw %", MMAPOnly | AllOnly},
	{"lrt|lwt", MMAPOnly | AllOnly},
	{"    locked db", Locks},
	{"qr|qw", Always},
	{"ar|aw", Always},
	{"netIn", Always},
	{"netOut", Always},
	{"conn", Always},
	{"set", Repl},
	{"repl", Repl},
	{"time", Always},
}

// NamespacedLocks stores information on the LockStatus of namespaces.
type NamespacedLocks map[string]LockStatus

// LockUsage stores information related to a namespace's lock usage.
type LockUsage struct {
	Namespace string
	Reads     int64
	Writes    int64
}

type lockUsages []LockUsage

func percentageInt64(value, outOf int64) float64 {
	if value == 0 || outOf == 0 {
		return 0
	}
	return 100 * (float64(value) / float64(outOf))
}

func averageInt64(value, outOf int64) int64 {
	if value == 0 || outOf == 0 {
		return 0
	}
	return value / outOf
}

func (slice lockUsages) Len() int {
	return len(slice)
}

func (slice lockUsages) Less(i, j int) bool {
	return slice[i].Reads+slice[i].Writes < slice[j].Reads+slice[j].Writes
}

func (slice lockUsages) Swap(i, j int) {
	slice[i], slice[j] = slice[j], slice[i]
}

// CollectionLockStatus stores a collection's lock statistics.
type CollectionLockStatus struct {
	ReadAcquireWaitsPercentage  float64
	WriteAcquireWaitsPercentage float64
	ReadAcquireTimeMicros       int64
	WriteAcquireTimeMicros      int64
}

// LockStatus stores a database's lock statistics.
type LockStatus struct {
	DBName     string
	Percentage float64
	Global     bool
}

// StatLine is a wrapper for all metrics reported by mongostat for monitored hosts.
type StatLine struct {
	Key string
	// What storage engine is being used for the node with this stat line
	StorageEngine string

	Error    error
	IsMongos bool
	Host     string

	// The time at which this StatLine was generated.
	Time time.Time

	// The last time at which this StatLine was printed to output.
	LastPrinted time.Time

	// Opcounter fields
	Insert, Query, Update, Delete, GetMore, Command int64

	// Collection locks (3.0 mmap only)
	CollectionLocks *CollectionLockStatus

	// Cache utilization (wiredtiger only)
	CacheDirtyPercent float64
	CacheUsedPercent  float64

	// Replicated Opcounter fields
	InsertR, QueryR, UpdateR, DeleteR, GetMoreR, CommandR int64
	Flushes                                               int64
	Mapped, Virtual, Resident, NonMapped                  int64
	Faults                                                int64
	HighestLocked                                         *LockStatus
	QueuedReaders, QueuedWriters                          int64
	ActiveReaders, ActiveWriters                          int64
	NetIn, NetOut                                         int64
	NumConnections                                        int64
	ReplSetName                                           string
	NodeType                                              string
}

func parseLocks(stat ServerStatus) map[string]LockUsage {
	returnVal := map[string]LockUsage{}
	for namespace, lockInfo := range stat.Locks {
		returnVal[namespace] = LockUsage{
			namespace,
			lockInfo.TimeLockedMicros.Read + lockInfo.TimeLockedMicros.ReadLower,
			lockInfo.TimeLockedMicros.Write + lockInfo.TimeLockedMicros.WriteLower,
		}
	}
	return returnVal
}

func computeLockDiffs(prevLocks, curLocks map[string]LockUsage) []LockUsage {
	lockUsages := lockUsages(make([]LockUsage, 0, len(curLocks)))
	for namespace, curUsage := range curLocks {
		prevUsage, hasKey := prevLocks[namespace]
		if !hasKey {
			// This namespace didn't appear in the previous batch of lock info,
			// so we can't compute a diff for it - skip it.
			continue
		}
		// Calculate diff of lock usage for this namespace and add to the list
		lockUsages = append(lockUsages,
			LockUsage{
				namespace,
				curUsage.Reads - prevUsage.Reads,
				curUsage.Writes - prevUsage.Writes,
			})
	}
	// Sort the array in order of least to most locked
	sort.Sort(lockUsages)
	return lockUsages
}

// formatOpcount returns a string for mongostat to display replset count
func formatOpcount(opcount, opcountRepl int64, both bool) string {
	if both {
		return fmt.Sprintf("%v|%v", opcount, opcountRepl)
	}
	if opcount > 0 && opcountRepl > 0 {
		return fmt.Sprintf("%v|%v", opcount, opcountRepl)
	} else if opcount > 0 {
		return fmt.Sprintf("%v", opcount)
	} else if opcountRepl > 0 {
		return fmt.Sprintf("*%v", opcountRepl)
	}
	return "*0"
}

// A LineFormatter formats StatLines for printing.
type LineFormatter interface {
	// FormatLines returns the string representation of the StatLines that
	// are passed in. It also takes a bool if cluster discovery is active.
	FormatLines(lines []StatLine, index int, discover bool) string
}

// Implementation of LineFormatter - converts the StatLines to JSON.
type JSONLineFormatter struct{}

// Satisfy the LineFormatter interface. Formats the StatLines as JSON.
func (jlf *JSONLineFormatter) FormatLines(lines []StatLine, index int, discover bool) string {

	lineFlags := getLineFlags(lines)

	// middle ground b/t the StatLines and the JSON string to be returned
	jsonFormat := map[string]interface{}{}

	// convert each StatLine to JSON
	for _, line := range lines {
		// each line can just be a string->string map (header->value)
		lineJson := map[string]string{}

		// check for error
		if line.Error != nil {
			lineJson["error"] = line.Error.Error()
			jsonFormat[line.Key] = lineJson
			continue
		}

		// put all the appropriate values into the stat line's JSON representation
		lineJson["insert"] = formatOpcount(line.Insert, line.InsertR, false)
		lineJson["query"] = formatOpcount(line.Query, line.QueryR, false)
		lineJson["update"] = formatOpcount(line.Update, line.UpdateR, false)
		lineJson["delete"] = formatOpcount(line.Delete, line.DeleteR, false)
		lineJson["getmore"] = fmt.Sprintf("%v", line.GetMore)
		lineJson["command"] = formatOpcount(line.Command, line.CommandR, true)
		lineJson["netIn"] = text.FormatBits(line.NetIn)
		lineJson["netOut"] = text.FormatBits(line.NetOut)
		lineJson["conn"] = fmt.Sprintf("%v", line.NumConnections)
		lineJson["time"] = fmt.Sprintf("%v", line.Time.Format("15:04:05"))
		lineJson["host"] = line.Host
		lineJson["vsize"] = text.FormatMegabyteAmount(int64(line.Virtual))
		lineJson["res"] = text.FormatMegabyteAmount(int64(line.Resident))

		// add mmapv1-specific fields
		if lineFlags&MMAPOnly > 0 {
			lineJson["flushes"] = fmt.Sprintf("%v", line.Flushes)
			lineJson["qr|qw"] = fmt.Sprintf("%v|%v", line.QueuedReaders,
				line.QueuedWriters)
			lineJson["ar|aw"] = fmt.Sprintf("%v|%v", line.ActiveReaders,
				line.ActiveWriters)

			mappedVal := ""      // empty for mongos
			if line.Mapped > 0 { // not mongos, update accordingly
				mappedVal = text.FormatMegabyteAmount(int64(line.Mapped))
			}
			lineJson["mapped"] = mappedVal

			nonMappedVal := ""       // empty for mongos
			if line.NonMapped >= 0 { // not mongos, update accordingly
				nonMappedVal = text.FormatMegabyteAmount(int64(line.NonMapped))
			}
			lineJson["non-mapped"] = nonMappedVal

			lineJson["faults"] = fmt.Sprintf("%v", line.Faults)

			if lineFlags&AllOnly > 0 {
				// check if we have any locks
				if lineFlags&Locks <= 0 {
					if line.CollectionLocks != nil && !line.IsMongos {
						lineJson["lr|lw %"] = fmt.Sprintf("%.1f%%|%.1f%%", line.CollectionLocks.ReadAcquireWaitsPercentage,
							line.CollectionLocks.WriteAcquireWaitsPercentage)
						lineJson["lrt|lwt"] = fmt.Sprintf("%v|%v", line.CollectionLocks.ReadAcquireTimeMicros,
							line.CollectionLocks.WriteAcquireTimeMicros)
					}
				}
			}

			highestLockedVal := "" // empty for mongos
			if line.HighestLocked != nil && !line.IsMongos {
				highestLockedVal = fmt.Sprintf("%v:%.1f%%",
					line.HighestLocked.DBName, line.HighestLocked.Percentage)
			}
			lineJson["locked"] = highestLockedVal
		}

		if lineFlags&Repl > 0 {
			lineJson["set"] = line.ReplSetName
			lineJson["repl"] = line.NodeType
		}

		// add the line to the final json
		jsonFormat[line.Host] = lineJson
	}

	// convert the JSON format of the lines to a json string to be returned
	linesAsJsonBytes, err := json.Marshal(jsonFormat)
	if err != nil {
		return fmt.Sprintf(`{"json error": "%v"}`, err.Error())
	}

	return string(linesAsJsonBytes) + "\n"
}

// Implementation of LineFormatter - uses a common/text.GridWriter to format
// the StatLines as a grid.
type GridLineFormatter struct {
	// If true, enables printing of headers to output
	IncludeHeader bool

	// Number of line outputs to skip between adding in headers
	HeaderInterval int

	// Grid writer
	Writer *text.GridWriter
}

// describes which sets of columns are printable in a StatLine
type lineAttributes struct {
	hasRepl  bool
	hasAll   bool
	hasLocks bool
}

func getLineFlags(lines []StatLine) int {
	flags := Always
	for _, line := range lines {
		if line.ReplSetName != "" || line.NodeType == "RTR" {
			flags |= Repl
		}
		if line.NonMapped >= 0 {
			flags |= AllOnly
		}
		if line.HighestLocked != nil {
			flags |= Locks
		}
		if line.StorageEngine == "mmapv1" {
			flags |= MMAPOnly
		}
		if line.CacheDirtyPercent >= 0 || line.CacheUsedPercent >= 0 {
			flags |= WTOnly
		}
	}
	return flags
}

// Satisfy the LineFormatter interface. Formats the StatLines as a grid.
func (glf *GridLineFormatter) FormatLines(lines []StatLine, index int, discover bool) string {
	buf := &bytes.Buffer{}

	// Automatically turn on discover-style formatting if more than one host's
	// output is being displayed (to include things like hostname column)
	discover = discover || len(lines) > 1

	if discover {
		glf.Writer.WriteCell(" ")
	}

	lineFlags := getLineFlags(lines)

	// Sort the stat lines by hostname, so that we see the output
	// in the same order for each snapshot
	sort.Sort(StatLines(lines))

	// Print the columns that are enabled
	for _, header := range StatHeaders {
		maskedAttrs := lineFlags & header.ActivateFlags
		// Only show the header if this column has the "Always" flag, or all
		// other flags for this column are matched
		if (maskedAttrs&Always == 0) && maskedAttrs != header.ActivateFlags {
			continue
		}

		// Don't write any cell content for blank headers, since they act as placeholders
		if len(header.HeaderText) > 0 {
			glf.Writer.WriteCell(header.HeaderText)
		}
	}
	glf.Writer.EndRow()

	for _, line := range lines {

		mmap := line.StorageEngine == "mmapv1"

		if discover {
			glf.Writer.WriteCell(line.Key)
		}
		if line.Error != nil {
			glf.Writer.Feed(line.Error.Error())
			continue
		}

		// Write the opcount columns (always active)
		glf.Writer.WriteCell(formatOpcount(line.Insert, line.InsertR, false))
		glf.Writer.WriteCell(formatOpcount(line.Query, line.QueryR, false))
		glf.Writer.WriteCell(formatOpcount(line.Update, line.UpdateR, false))
		glf.Writer.WriteCell(formatOpcount(line.Delete, line.DeleteR, false))
		glf.Writer.WriteCell(fmt.Sprintf("%v", line.GetMore))
		glf.Writer.WriteCell(formatOpcount(line.Command, line.CommandR, true))

		if lineFlags&WTOnly > 0 {
			if line.CacheDirtyPercent < 0 {
				glf.Writer.WriteCell("")
			} else {
				glf.Writer.WriteCell(fmt.Sprintf("%.1f", line.CacheDirtyPercent*100))
			}
			if line.CacheUsedPercent < 0 {
				glf.Writer.WriteCell("")
			} else {
				glf.Writer.WriteCell(fmt.Sprintf("%.1f", line.CacheUsedPercent*100))
			}
		}

		glf.Writer.WriteCell(fmt.Sprintf("%v", line.Flushes))

		// Columns for flushes + mapped only show up if mmap columns are active
		if lineFlags&MMAPOnly > 0 {

			if line.Mapped > 0 {
				glf.Writer.WriteCell(text.FormatMegabyteAmount(int64(line.Mapped)))
			} else {
				//for mongos nodes, Mapped is empty, so write a blank cell.
				glf.Writer.WriteCell("")
			}
		}

		// Columns for Virtual and Resident are always active
		glf.Writer.WriteCell(text.FormatMegabyteAmount(int64(line.Virtual)))
		glf.Writer.WriteCell(text.FormatMegabyteAmount(int64(line.Resident)))

		if lineFlags&MMAPOnly > 0 {
			if lineFlags&AllOnly > 0 {
				nonMappedVal := ""
				if line.NonMapped >= 0 { // not mongos, update accordingly
					nonMappedVal = text.FormatMegabyteAmount(int64(line.NonMapped))
				}
				glf.Writer.WriteCell(nonMappedVal)
			}
			if mmap {
				glf.Writer.WriteCell(fmt.Sprintf("%v", line.Faults))
			} else {
				glf.Writer.WriteCell("n/a")
			}
		}

		if lineFlags&MMAPOnly > 0 && lineFlags&AllOnly > 0 {
			// check if we have any locks
			if lineFlags&Locks <= 0 {
				if line.CollectionLocks != nil && !line.IsMongos {
					percentCell := fmt.Sprintf("%.1f%%|%.1f%%", line.CollectionLocks.ReadAcquireWaitsPercentage,
						line.CollectionLocks.WriteAcquireWaitsPercentage)
					glf.Writer.WriteCell(percentCell)
					timeCell := fmt.Sprintf("%v|%v", line.CollectionLocks.ReadAcquireTimeMicros,
						line.CollectionLocks.WriteAcquireTimeMicros)
					glf.Writer.WriteCell(timeCell)
				} else {
					//don't write any lock status for mongos nodes
					glf.Writer.WriteCell("")
					glf.Writer.WriteCell("")
				}
			} else {
				// no locks
				glf.Writer.WriteCell("n/a")
				glf.Writer.WriteCell("n/a")
			}
		}

		// Write columns related to lock % if activated
		if lineFlags&Locks > 0 {
			if line.HighestLocked != nil && !line.IsMongos {
				lockCell := fmt.Sprintf("%v:%.1f", line.HighestLocked.DBName,
					line.HighestLocked.Percentage) + "%"
				glf.Writer.WriteCell(lockCell)
			} else {
				//don't write any lock status for mongos nodes
				glf.Writer.WriteCell("")
			}
		}
		glf.Writer.WriteCell(fmt.Sprintf("%v|%v", line.QueuedReaders, line.QueuedWriters))
		glf.Writer.WriteCell(fmt.Sprintf("%v|%v", line.ActiveReaders, line.ActiveWriters))

		glf.Writer.WriteCell(text.FormatBits(line.NetIn))
		glf.Writer.WriteCell(text.FormatBits(line.NetOut))

		glf.Writer.WriteCell(fmt.Sprintf("%v", line.NumConnections))
		if discover || lineFlags&Repl > 0 { //only show these fields when in discover or repl mode.
			glf.Writer.WriteCell(line.ReplSetName)
			glf.Writer.WriteCell(line.NodeType)
		}

		glf.Writer.WriteCell(fmt.Sprintf("%v", line.Time.Format("2006-01-02T15:04:05Z07:00")))
		glf.Writer.EndRow()
	}
	glf.Writer.Flush(buf)

	// clear the flushed data
	glf.Writer.Reset()

	returnVal := buf.String()

	if !glf.IncludeHeader || index%glf.HeaderInterval != 0 {
		// Strip out the first line of the formatted output,
		// which contains the headers. They've been left in up until this point
		// in order to force the formatting of the columns to be wide enough.
		firstNewLinePos := strings.Index(returnVal, "\n")
		if firstNewLinePos >= 0 {
			returnVal = returnVal[firstNewLinePos+1:]
		}
	}

	if len(lines) > 1 {
		// For multi-node stats, add an extra newline to tell each block apart
		return "\n" + returnVal
	}
	return returnVal
}

func diff(newVal, oldVal, sampleTime int64) int64 {
	return (newVal - oldVal) / sampleTime
}

// NewStatLine constructs a StatLine object from two ServerStatus objects.
func NewStatLine(oldStat, newStat ServerStatus, key string, all bool, sampleSecs int64) *StatLine {
	returnVal := &StatLine{
		Key:       key,
		Host:      newStat.Host,
		Mapped:    -1,
		Virtual:   -1,
		Resident:  -1,
		NonMapped: -1,
		Faults:    -1,
	}

	// set the storage engine appropriately
	if newStat.StorageEngine != nil && newStat.StorageEngine["name"] != "" {
		returnVal.StorageEngine = newStat.StorageEngine["name"]
	} else {
		returnVal.StorageEngine = "mmapv1"
	}

	if newStat.Opcounters != nil && oldStat.Opcounters != nil {
		returnVal.Insert = diff(newStat.Opcounters.Insert, oldStat.Opcounters.Insert, sampleSecs)
		returnVal.Query = diff(newStat.Opcounters.Query, oldStat.Opcounters.Query, sampleSecs)
		returnVal.Update = diff(newStat.Opcounters.Update, oldStat.Opcounters.Update, sampleSecs)
		returnVal.Delete = diff(newStat.Opcounters.Delete, oldStat.Opcounters.Delete, sampleSecs)
		returnVal.GetMore = diff(newStat.Opcounters.GetMore, oldStat.Opcounters.GetMore, sampleSecs)
		returnVal.Command = diff(newStat.Opcounters.Command, oldStat.Opcounters.Command, sampleSecs)
	}

	if newStat.OpcountersRepl != nil && oldStat.OpcountersRepl != nil {
		returnVal.InsertR = diff(newStat.OpcountersRepl.Insert, oldStat.OpcountersRepl.Insert, sampleSecs)
		returnVal.QueryR = diff(newStat.OpcountersRepl.Query, oldStat.OpcountersRepl.Query, sampleSecs)
		returnVal.UpdateR = diff(newStat.OpcountersRepl.Update, oldStat.OpcountersRepl.Update, sampleSecs)
		returnVal.DeleteR = diff(newStat.OpcountersRepl.Delete, oldStat.OpcountersRepl.Delete, sampleSecs)
		returnVal.GetMoreR = diff(newStat.OpcountersRepl.GetMore, oldStat.OpcountersRepl.GetMore, sampleSecs)
		returnVal.CommandR = diff(newStat.OpcountersRepl.Command, oldStat.OpcountersRepl.Command, sampleSecs)
	}

	returnVal.CacheDirtyPercent = -1
	returnVal.CacheUsedPercent = -1
	if newStat.WiredTiger != nil && oldStat.WiredTiger != nil {
		returnVal.Flushes = newStat.WiredTiger.Transaction.TransCheckpoints - oldStat.WiredTiger.Transaction.TransCheckpoints
		returnVal.CacheDirtyPercent = float64(newStat.WiredTiger.Cache.TrackedDirtyBytes) / float64(newStat.WiredTiger.Cache.MaxBytesConfigured)
		returnVal.CacheUsedPercent = float64(newStat.WiredTiger.Cache.CurrentCachedBytes) / float64(newStat.WiredTiger.Cache.MaxBytesConfigured)
	} else if newStat.BackgroundFlushing != nil && oldStat.BackgroundFlushing != nil {
		returnVal.Flushes = newStat.BackgroundFlushing.Flushes - oldStat.BackgroundFlushing.Flushes
	}

	returnVal.Time = newStat.SampleTime
	returnVal.IsMongos =
		(newStat.ShardCursorType != nil || strings.HasPrefix(newStat.Process, MongosProcess))

	if util.IsTruthy(oldStat.Mem.Supported) {
		if !returnVal.IsMongos {
			returnVal.Mapped = newStat.Mem.Mapped
		}
		returnVal.Virtual = newStat.Mem.Virtual
		returnVal.Resident = newStat.Mem.Resident

		if !returnVal.IsMongos && all {
			returnVal.NonMapped = newStat.Mem.Virtual - newStat.Mem.Mapped
		}
	}

	if newStat.Repl != nil {
		setName, isReplSet := newStat.Repl.SetName.(string)
		if isReplSet {
			returnVal.ReplSetName = setName
		}
		if util.IsTruthy(newStat.Repl.IsMaster) {
			returnVal.NodeType = "PRI"
		} else if util.IsTruthy(newStat.Repl.Secondary) {
			returnVal.NodeType = "SEC"
		} else if util.IsTruthy(newStat.Repl.IsReplicaSet) {
			returnVal.NodeType = "REC"
		} else if util.IsTruthy(newStat.Repl.ArbiterOnly) {
			returnVal.NodeType = "ARB"
		} else if util.SliceContains(newStat.Repl.Passives, newStat.Repl.Me) {
			returnVal.NodeType = "PSV"
		} else if isReplSet {
			returnVal.NodeType = "UNK"
		} else {
			returnVal.NodeType = "SLV"
		}
	} else if returnVal.IsMongos {
		returnVal.NodeType = "RTR"
	}

	if oldStat.ExtraInfo != nil && newStat.ExtraInfo != nil &&
		oldStat.ExtraInfo.PageFaults != nil && newStat.ExtraInfo.PageFaults != nil {
		returnVal.Faults = diff(*(newStat.ExtraInfo.PageFaults), *(oldStat.ExtraInfo.PageFaults), sampleSecs)
	}
	if !returnVal.IsMongos && oldStat.Locks != nil && oldStat.Locks != nil {
		globalCheck, hasGlobal := oldStat.Locks["Global"]
		if hasGlobal && globalCheck.AcquireCount != nil {
			// This appears to be a 3.0+ server so the data in these fields do *not* refer to
			// actual namespaces and thus we can't compute lock %.
			returnVal.HighestLocked = nil

			// Check if it's a 3.0+ MMAP server so we can still compute collection locks
			collectionCheck, hasCollection := oldStat.Locks["Collection"]
			if hasCollection && collectionCheck.AcquireWaitCount != nil {
				readWaitCountDiff := newStat.Locks["Collection"].AcquireWaitCount.Read - oldStat.Locks["Collection"].AcquireWaitCount.Read
				readTotalCountDiff := newStat.Locks["Collection"].AcquireCount.Read - oldStat.Locks["Collection"].AcquireCount.Read
				writeWaitCountDiff := newStat.Locks["Collection"].AcquireWaitCount.Write - oldStat.Locks["Collection"].AcquireWaitCount.Write
				writeTotalCountDiff := newStat.Locks["Collection"].AcquireCount.Write - oldStat.Locks["Collection"].AcquireCount.Write
				readAcquireTimeDiff := newStat.Locks["Collection"].TimeAcquiringMicros.Read - oldStat.Locks["Collection"].TimeAcquiringMicros.Read
				writeAcquireTimeDiff := newStat.Locks["Collection"].TimeAcquiringMicros.Write - oldStat.Locks["Collection"].TimeAcquiringMicros.Write
				returnVal.CollectionLocks = &CollectionLockStatus{
					ReadAcquireWaitsPercentage:  percentageInt64(readWaitCountDiff, readTotalCountDiff),
					WriteAcquireWaitsPercentage: percentageInt64(writeWaitCountDiff, writeTotalCountDiff),
					ReadAcquireTimeMicros:       averageInt64(readAcquireTimeDiff, readWaitCountDiff),
					WriteAcquireTimeMicros:      averageInt64(writeAcquireTimeDiff, writeWaitCountDiff),
				}
			}
		} else {
			prevLocks := parseLocks(oldStat)
			curLocks := parseLocks(newStat)
			lockdiffs := computeLockDiffs(prevLocks, curLocks)
			if len(lockdiffs) == 0 {
				if newStat.GlobalLock != nil {
					returnVal.HighestLocked = &LockStatus{
						DBName:     "",
						Percentage: percentageInt64(newStat.GlobalLock.LockTime, newStat.GlobalLock.TotalTime),
						Global:     true,
					}
				}
			} else {
				// Get the entry with the highest lock
				highestLocked := lockdiffs[len(lockdiffs)-1]

				var timeDiffMillis int64
				timeDiffMillis = newStat.UptimeMillis - oldStat.UptimeMillis

				lockToReport := highestLocked.Writes

				// if the highest locked namespace is not '.'
				if highestLocked.Namespace != "." {
					for _, namespaceLockInfo := range lockdiffs {
						if namespaceLockInfo.Namespace == "." {
							lockToReport += namespaceLockInfo.Writes
						}
					}
				}

				// lock data is in microseconds and uptime is in milliseconds - so
				// divide by 1000 so that they units match
				lockToReport /= 1000

				returnVal.HighestLocked = &LockStatus{
					DBName:     highestLocked.Namespace,
					Percentage: percentageInt64(lockToReport, timeDiffMillis),
					Global:     false,
				}
			}
		}
	} else {
		returnVal.HighestLocked = nil
	}

	if newStat.GlobalLock != nil {
		hasWT := (newStat.WiredTiger != nil && oldStat.WiredTiger != nil)
		//If we have wiredtiger stats, use those instead
		if newStat.GlobalLock.CurrentQueue != nil {
			if hasWT {
				returnVal.QueuedReaders = newStat.GlobalLock.CurrentQueue.Readers + newStat.GlobalLock.ActiveClients.Readers - newStat.WiredTiger.Concurrent.Read.Out
				returnVal.QueuedWriters = newStat.GlobalLock.CurrentQueue.Writers + newStat.GlobalLock.ActiveClients.Writers - newStat.WiredTiger.Concurrent.Write.Out
				if returnVal.QueuedReaders < 0 {
					returnVal.QueuedReaders = 0
				}
				if returnVal.QueuedWriters < 0 {
					returnVal.QueuedWriters = 0
				}
			} else {
				returnVal.QueuedReaders = newStat.GlobalLock.CurrentQueue.Readers
				returnVal.QueuedWriters = newStat.GlobalLock.CurrentQueue.Writers
			}
		}

		if hasWT {
			returnVal.ActiveReaders = newStat.WiredTiger.Concurrent.Read.Out
			returnVal.ActiveWriters = newStat.WiredTiger.Concurrent.Write.Out
		} else if newStat.GlobalLock.ActiveClients != nil {
			returnVal.ActiveReaders = newStat.GlobalLock.ActiveClients.Readers
			returnVal.ActiveWriters = newStat.GlobalLock.ActiveClients.Writers
		}
	}

	if oldStat.Network != nil && newStat.Network != nil {
		returnVal.NetIn = diff(newStat.Network.BytesIn, oldStat.Network.BytesIn, sampleSecs)
		returnVal.NetOut = diff(newStat.Network.BytesOut, oldStat.Network.BytesOut, sampleSecs)
	}

	if newStat.Connections != nil {
		returnVal.NumConnections = newStat.Connections.Current
	}

	return returnVal
}

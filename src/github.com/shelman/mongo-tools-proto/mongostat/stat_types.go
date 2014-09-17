package mongostat

import (
	"bytes"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/text"
	"github.com/shelman/mongo-tools-proto/common/util"
	"sort"
	"strings"
	//"text/tabwriter"
	"time"
)

const (
	MongodProcess = "mongod"
	MongosProcess = "mongos"
)

type StatLines []StatLine

func (slice StatLines) Len() int {
	return len(slice)
}

func (slice StatLines) Less(i, j int) bool {
	return slice[i].Host < slice[j].Host
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
	Uptime             int                    `bson:"uptime"`
	UptimeMillis       int64                  `bson:"uptimeMillis"`
	UptimeEstimate     int                    `bson:"uptimeEstimate"`
	LocalTime          time.Time              `bson:"localTime"`
	Asserts            map[string]int         `bson:"asserts"`
	BackgroundFlushing *FlushStats            `bson:"backgroundFlushing"`
	ExtraInfo          *ExtraInfo             `bson:"extra_info"`
	Connections        *ConnectionStats       `bson:"connections"`
	Dur                *DurStats              `bson:"dur"`
	GlobalLock         *GlobalLockStats       `bson:"globalLock"`
	IndexCounter       *IndexCounterStats     `bson:"indexCounters"`
	Locks              map[string]LockStats   `bson:"locks"`
	Network            *NetworkStats          `bson:"network"`
	Opcounters         *OpcountStats          `bson:"opcounters"`
	OpcountersRepl     *OpcountStats          `bson:"opcountersRepl"`
	RecordStats        *DBRecordStats         `bson:"recordStats"`
	Mem                *MemStats              `bson:"mem"`
	Repl               *ReplStatus            `bson:"repl"`
	ShardCursorType    map[string]interface{} `bson:"shardCursorType"`
}

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

type DBRecordStats struct {
	AccessesNotInMemory       int                       `bson:"accessesNotInMemory"`
	PageFaultExceptionsThrown int                       `bson:"pageFaultExceptionsThrown"`
	DBRecordAccesses          map[string]RecordAccesses `bson:",inline"`
}

type RecordAccesses struct {
	AccessesNotInMemory       int `bson:"accessesNotInMemory"`
	PageFaultExceptionsThrown int `bson:"pageFaultExceptionsThrown"`
}

type MemStats struct {
	Bits              int64       `bson:"bits"`
	Resident          int64       `bson:"resident"`
	Virtual           int64       `bson:"virtual"`
	Supported         interface{} `bson:"supported"`
	Mapped            int64       `bson:"mapped"`
	MappedWithJournal int64       `bson:"mappedWithJournal"`
}

type FlushStats struct {
	Flushes      int       `bson:"flushes"`
	TotalMs      int       `bson:"total_ms"`
	AverageMs    float64   `bson:"average_ms"`
	LastMs       int       `bson:"last_ms"`
	LastFinished time.Time `bson:"last_finished"`
}

type ConnectionStats struct {
	Current      int   `bson:"current"`
	Available    int   `bson:"available"`
	TotalCreated int64 `bson:"totalCreated"`
}

type DurTiming struct {
	Dt               int `bson:"dt"`
	PrepLogBuffer    int `bson:"prepLogBuffer"`
	WriteToJournal   int `bson:"writeToJournal"`
	WriteToDataFiles int `bson:"writeToDataFiles"`
	RemapPrivateView int `bson:"remapPrivateView"`
}

type DurStats struct {
	Commits            int `bson:"commits"`
	JournaledMB        int `bson:"journaledMB"`
	WriteToDataFilesMB int `bson:"writeToDataFilesMB"`
	Compression        int `bson:"compression"`
	CommitsInWriteLock int `bson:"commitsInWriteLock"`
	EarlyCommits       int `bson:"earlyCommits"`
	TimeMs             DurTiming
}
type QueueStats struct {
	Total   int `bson:"total"`
	Readers int `bson:"readers"`
	Writers int `bson:"writers"`
}

type ClientStats struct {
	Total   int `bson:"total"`
	Readers int `bson:"readers"`
	Writers int `bson:"writers"`
}

type GlobalLockStats struct {
	TotalTime     int64        `bson:"totalTime"`
	LockTime      int64        `bson:"lockTime"`
	CurrentQueue  *QueueStats  `bson:"currentQueue"`
	ActiveClients *ClientStats `bson:"activeClients"`
}

type IndexCounterStats struct {
	Accesses  int `bson:"accesses"`
	Hits      int `bson:"hits"`
	Misses    int `bson:"misses"`
	Resets    int `bson:"resets"`
	MissRatio int `bson:"missRatio"`
}

type NetworkStats struct {
	BytesIn     int64 `bson:"bytesIn"`
	BytesOut    int64 `bson:"bytesOut"`
	NumRequests int   `bson:"numRequests"`
}

type OpcountStats struct {
	Insert  int `bson:"insert"`
	Query   int `bson:"query"`
	Update  int `bson:"update"`
	Delete  int `bson:"delete"`
	GetMore int `bson:"getMore"`
	Command int `bson:"command"`
}

type ReadWriteLockTimes struct {
	Read       int64 `bson:"R"`
	Write      int64 `bson:"W"`
	ReadLower  int64 `bson:"r"`
	WriteLower int64 `bson:"w"`
}

type LockStats struct {
	TimeLockedMicros    ReadWriteLockTimes `bson:"timeLockedMicros"`
	TimeAcquiringMicros ReadWriteLockTimes `bson:"timeAcquiringMicros"`
}

type ExtraInfo struct {
	PageFaults *int `bson:"page_faults"`
}

//StatHeader describes a single column for mongostat's terminal output,
//its formatting, and in which modes it should be displayed
type StatHeader struct {
	//The text to appear in the column's header cell
	HeaderText string

	//Indicates that the column should only appear when mongostat
	//is being used with --discover.
	DiscoverOnly bool

	//Indicates that the column should only appear when some of the nodes being
	//monitored are members of a replicaset.
	ReplOnly bool
}

var StatHeaders = []StatHeader{
	{"", true, false}, //placeholder for hostname column (blank header text)
	{"insert", false, false},
	{"query", false, false},
	{"update", false, false},
	{"delete", false, false},
	{"getmore", false, false},
	{"command", false, false},
	{"flushes", false, false},
	{"mapped", false, false},
	{"vsize", false, false},
	{"res", false, false},
	{"faults", false, false},
	{"    locked db", false, false},
	{"idx miss %", false, false},
	{"qr|qw", false, false},
	{"ar|aw", false, false},
	{"netIn", false, false},
	{"netOut", false, false},
	{"conn", false, false},
	{"set", true, true},
	{"repl", true, true},
	{"time", false, false},
}

type NamespacedLocks map[string]LockStatus

type LockUsage struct {
	Namespace string
	Reads     int64
	Writes    int64
}

type LockUsages []LockUsage

func percentageInt64(value, outOf int64) float64 {
	if value == 0 || outOf == 0 {
		return 0
	}
	return 100 * (float64(value) / float64(outOf))
}

func (slice LockUsages) Len() int {
	return len(slice)
}

func (slice LockUsages) Less(i, j int) bool {
	return slice[i].Reads+slice[i].Writes < slice[j].Reads+slice[j].Writes
}

func (slice LockUsages) Swap(i, j int) {
	slice[i], slice[j] = slice[j], slice[i]
}

func formatMegs(size int64) string {
	result := float64(size)
	unit := "m"
	if result > 1024 {
		unit = "g"
		result = result / 1024
	}
	return fmt.Sprintf("%.1f%v", result, unit)
}

func formatNet(diff int64) string {
	div := int64(1000)
	unit := "b"

	if diff >= div {
		unit = "k"
		diff /= div
	}

	if diff >= div {
		unit = "m"
		diff /= div
	}

	if diff >= div {
		unit = "g"
		diff /= div
	}
	return fmt.Sprintf("%v%v", diff, unit)

}

type LockStatus struct {
	DBName     string
	Percentage float64
	Global     bool
}

type StatLine struct {
	IsMongos bool
	Host     string

	//Opcounter fields
	Insert, Query, Update, Delete, GetMore, Command int
	Flushes                                         int
	Mapped, Virtual, Resident, NonMapped            int64
	Faults                                          int
	HighestLocked                                   *LockStatus
	IndexMissPercent                                float64
	QueuedReaders, QueuedWriters                    int
	ActiveReaders, ActiveWriters                    int
	NetIn, NetOut                                   int64
	NumConnections                                  int
	Time                                            time.Time
	ReplSetName                                     string
	NodeType                                        string
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
	lockUsages := LockUsages(make([]LockUsage, 0, len(curLocks)))
	for namespace, curUsage := range curLocks {
		prevUsage, hasKey := prevLocks[namespace]
		if !hasKey {
			//This namespace didn't appear in the previous batch of lock info,
			//so we can't compute a diff for it - skip it.
			continue
		}
		//Calculate diff of lock usage for this namespace and add to the list
		lockUsages = append(lockUsages,
			LockUsage{
				namespace,
				curUsage.Reads - prevUsage.Reads,
				curUsage.Writes - prevUsage.Writes,
			})
	}
	//Sort the array in order of least to most locked
	sort.Sort(lockUsages)
	return lockUsages
}

func FormatLines(lines []StatLine, includeHeader bool, discover bool) string {
	buf := &bytes.Buffer{}
	out := &text.GridWriter{ColumnPadding: 1}

	if discover {
		out.WriteCell(" ")
	}

	repl := false
	// if any of the nodes being monitored are part of a replset,
	// enable the printing of replset-specific columns
	for _, line := range lines {
		if line.ReplSetName != "" || line.NodeType == "RTR" {
			repl = true
		}
	}

	// Sort the stat lines by hostname, so that we see the output
	// in the same order for each snapshot
	sort.Sort(StatLines(lines))

	//Print the columns that are enabled
	for _, header := range StatHeaders {
		if (!header.ReplOnly && !header.DiscoverOnly) || //Always enabled?
			(repl && header.ReplOnly) || //Only show for repl, and in repl mode?
			(discover && header.DiscoverOnly) { //Only show for discover, and in discover mode?
			if len(header.HeaderText) > 0 {
				out.WriteCell(header.HeaderText)
			}
		}
	}
	out.EndRow()

	for _, line := range lines {
		if discover {
			out.WriteCell(line.Host)
		}

		out.WriteCell(fmt.Sprintf("%v", line.Insert))
		out.WriteCell(fmt.Sprintf("%v", line.Query))
		out.WriteCell(fmt.Sprintf("%v", line.Update))
		out.WriteCell(fmt.Sprintf("%v", line.Delete))
		out.WriteCell(fmt.Sprintf("%v", line.GetMore))
		out.WriteCell(fmt.Sprintf("%v", line.Command))
		out.WriteCell(fmt.Sprintf("%v", line.Flushes))
		if line.Mapped > 0 {
			out.WriteCell(formatMegs(int64(line.Mapped)))
		} else {
			//for mongos nodes, Mapped is empty, so write a blank cell.
			out.WriteCell("")
		}
		out.WriteCell(formatMegs(int64(line.Virtual)))
		out.WriteCell(formatMegs(int64(line.Resident)))
		out.WriteCell(fmt.Sprintf("%v", line.Faults))
		if line.HighestLocked != nil && !line.IsMongos {
			out.WriteCell(fmt.Sprintf("%v:%.1f%%", line.HighestLocked.DBName, line.HighestLocked.Percentage))
		} else {
			//don't write any lock status for mongos nodes
			out.WriteCell("")
		}
		out.WriteCell(fmt.Sprintf("%v", line.IndexMissPercent))
		out.WriteCell(fmt.Sprintf("%v|%v", line.QueuedReaders, line.QueuedWriters))
		out.WriteCell(fmt.Sprintf("%v|%v", line.ActiveReaders, line.ActiveWriters))
		out.WriteCell(formatNet(line.NetIn))
		out.WriteCell(formatNet(line.NetOut))
		out.WriteCell(fmt.Sprintf("%v", line.NumConnections))
		if discover || repl { //only show these fields when in discover or repl mode.
			out.WriteCell(line.ReplSetName)
			out.WriteCell(line.NodeType)
		}

		out.WriteCell(fmt.Sprintf("%v", line.Time.Format("15:04:05")))
		out.EndRow()
	}
	out.Flush(buf)
	returnVal := buf.String()

	if !includeHeader {
		//Strip out the first line of the formatted output,
		//which contains the headers. They've been left in up until this point
		//in order to force the formatting of the columns to be wide enough.
		firstNewLinePos := strings.Index(returnVal, "\n")
		if firstNewLinePos >= 0 {
			returnVal = returnVal[firstNewLinePos+1:]
		}
	}

	if len(lines) > 1 {
		//For multi-node stats, add an extra newline to tell each block apart
		return "\n" + returnVal
	}
	return returnVal
}

//NewStatLine constructs a StatLine object from two ServerStatus objects.
func NewStatLine(oldStat, newStat ServerStatus, all bool) *StatLine {
	returnVal := &StatLine{
		Host:      newStat.Host,
		Mapped:    -1,
		Virtual:   -1,
		Resident:  -1,
		NonMapped: -1,
		Faults:    -1,
	}

	if newStat.Opcounters != nil && oldStat.Opcounters != nil {
		returnVal.Insert = newStat.Opcounters.Insert - oldStat.Opcounters.Insert
		returnVal.Query = newStat.Opcounters.Query - oldStat.Opcounters.Query
		returnVal.Update = newStat.Opcounters.Update - oldStat.Opcounters.Update
		returnVal.Delete = newStat.Opcounters.Delete - oldStat.Opcounters.Delete
		returnVal.GetMore = newStat.Opcounters.GetMore - oldStat.Opcounters.GetMore
		returnVal.Command = newStat.Opcounters.Command - oldStat.Opcounters.Command
	}
	if newStat.BackgroundFlushing != nil && oldStat.BackgroundFlushing != nil {
		returnVal.Flushes = newStat.BackgroundFlushing.Flushes - oldStat.BackgroundFlushing.Flushes
	}
	returnVal.Time = newStat.SampleTime
	returnVal.IsMongos =
		(newStat.ShardCursorType != nil || newStat.Process == MongosProcess)

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
		} else if util.SliceContains(newStat.Repl.Me, newStat.Repl.Passives) {
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
		returnVal.Faults = *(newStat.ExtraInfo.PageFaults) - *(oldStat.ExtraInfo.PageFaults)
	}
	if !returnVal.IsMongos {
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
		}

		if len(lockdiffs) > 0 {
			//Get the entry with the highest lock
			highestLocked := lockdiffs[len(lockdiffs)-1]

			//TODO use server uptime since the previous sampling?
			//var timeDiffMillis int64
			//timeDiffMillis = newStat.UptimeMillis - stat.UptimeMillis

			lockToReport := highestLocked.Writes

			//if the highest locked namespace is not '.'
			if highestLocked.Namespace != "." {
				for _, namespaceLockInfo := range lockdiffs {
					if namespaceLockInfo.Namespace == "." {
						lockToReport += namespaceLockInfo.Writes
					}
				}
			}

			//lock data is in microseconds and uptime is in milliseconds - so
			//divide by 1000 so that they units match
			lockToReport /= 1000

			returnVal.HighestLocked = &LockStatus{
				DBName:     highestLocked.Namespace,
				Percentage: percentageInt64(lockToReport, 1000),
				Global:     false,
			}
		}
	} else {
		returnVal.HighestLocked = nil
	}

	if newStat.GlobalLock != nil {
		if newStat.GlobalLock.CurrentQueue != nil {
			returnVal.QueuedReaders = newStat.GlobalLock.CurrentQueue.Readers
			returnVal.QueuedWriters = newStat.GlobalLock.CurrentQueue.Writers
		}

		if newStat.GlobalLock.ActiveClients != nil {
			returnVal.ActiveReaders = newStat.GlobalLock.ActiveClients.Readers
			returnVal.ActiveWriters = newStat.GlobalLock.ActiveClients.Writers
		}
	}

	if oldStat.Network != nil && newStat.Network != nil {
		returnVal.NetIn = newStat.Network.BytesIn - oldStat.Network.BytesIn
		returnVal.NetOut = newStat.Network.BytesOut - oldStat.Network.BytesOut
	}

	if newStat.Connections != nil {
		returnVal.NumConnections = newStat.Connections.Current
	}

	return returnVal
}

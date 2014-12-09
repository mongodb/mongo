package mongostat

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"strings"
	"sync"
	"time"
)

const storageEngineWarning = `Warning: not all columns will apply to mongods running storage engines other than mmapv1.`

//MongoStat is a container for the user-specified options and
//internal cluster state used for running mongostat.
type MongoStat struct {
	// generic mongo tool options
	Options *options.ToolOptions

	// mongostat-specific output options
	StatOptions *StatOptions

	//How long to sleep between printing the rows, and polling the server
	SleepInterval time.Duration

	//New nodes can be "discovered" by any other node by sending a hostname
	//on this channel.
	Discovered chan string

	//A map of hostname -> NodeMonitor for all the hosts that
	//are being monitored.
	Nodes map[string]*NodeMonitor

	//ClusterMonitor to manage collecting and printing the stats from all nodes
	Cluster ClusterMonitor

	//Mutex to handle safe concurrent adding to or looping over discovered nodes
	nodesLock sync.RWMutex
}

type ConfigShard struct {
	Id   string `bson:"_id"`
	Host string `bson:"host"`
}

//NodeMonitor is a struct that contains the connection pool for a single host
//and collects the mongostat data for that host on a regular interval
type NodeMonitor struct {
	host            string
	sessionProvider *db.SessionProvider

	//Enable/Disable collection of optional fields
	All bool

	//The previous result of the ServerStatus command used to calculate diffs.
	LastStatus *ServerStatus

	//The time at which the node monitor last processed an update successfully
	LastUpdate time.Time

	//The most recent err encountered when collecting stats for this node
	Err error
}

//SyncClusterMonitor is an implementation of ClusterMonitor that writes output
//synchronized with the timing of when the polling samples are collected.
//Only works with a single host at a time.
type SyncClusterMonitor struct {
	//Channel to listen for incoming stat data
	ReportChan chan StatLine

	//Used to format the StatLines for printing
	Formatter LineFormatter
}

type StatOutputOptions struct {
	//Disable printing of column headers
	NoHeaders bool
}

//ClusterMonitor maintains an internal representation of a cluster's state,
//which can be refreshed with calls to Update(), and dumps output representing
//this internal state on an interval.
type ClusterMonitor interface {
	//Monitor() triggers monitoring and dumping output to begin
	//maxRows is the number of times to dump output before exiting. If <0,
	//Monitor() will run indefinitely.
	//done is a channel to send an error if one is encountered. A nil value will
	//be sent on this channel if Monitor() completes with no error.
	//sleep is the interval to sleep between output dumps.
	Monitor(maxRows int, done chan error, sleep time.Duration)

	//Update signals the ClusterMonitor implementation to refresh its internal
	//state using the data contained in the provided StatLine.
	Update(statLine StatLine)
}

type AsyncClusterMonitor struct {
	Discover bool

	//Channel to listen for incoming stat data
	ReportChan chan StatLine

	//Map of hostname -> latest stat data for the host
	LastStatLines map[string]*StatLine

	//Mutex to protect access to LastStatLines
	mapLock sync.Mutex

	//Used to format the StatLines for printing
	Formatter LineFormatter

	//Whether or not the last group included any nodes running non-mmap
	//storage engines
	lastHadNonMmap bool
}

//Update() refreshes the internal state of the cluster monitor with the data
//in the StatLine.  SyncClusterMonitor's implementation of Update() blocks
//until it has written out its state, so that output is always dumped exactly
//once for each poll.
func (cluster *SyncClusterMonitor) Update(statLine StatLine) {
	cluster.ReportChan <- statLine
}

func (cluster *SyncClusterMonitor) Monitor(maxRows int, done chan error, sleep time.Duration) {
	go func() {
		rowCount := 0
		warned := false
		hasData := false
		for {
			newStat := <-cluster.ReportChan
			if newStat.Error != nil && !hasData {
				done <- newStat.Error
				return
			}
			hasData = true

			//If this mongod is running a storage engine other than mmapv1,
			//print a warning about the non-applicable fields.
			//Only display the warning once.
			if newStat.Error == nil && newStat.StorageEngine != "mmapv1" && !warned {
				log.Log(log.Always, storageEngineWarning)
				warned = true
			}

			out := cluster.Formatter.FormatLines([]StatLine{newStat}, rowCount, false)
			fmt.Print(out)
			rowCount++
			if maxRows > 0 && rowCount >= maxRows {
				break
			}
		}
		done <- nil
	}()
}

//updateHostInfo updates the internal map with the given StatLine data.
//Safe for concurrent access.
func (cluster *AsyncClusterMonitor) updateHostInfo(stat StatLine) {
	cluster.mapLock.Lock()
	defer cluster.mapLock.Unlock()
	cluster.LastStatLines[stat.Key] = &stat
}

//printSnapshot formats + dumps the current state of all the stats collected
func (cluster *AsyncClusterMonitor) printSnapshot(lineCount int, discover bool) {
	cluster.mapLock.Lock()
	defer cluster.mapLock.Unlock()
	lines := make([]StatLine, 0, len(cluster.LastStatLines))
	for _, stat := range cluster.LastStatLines {
		if stat.LastPrinted == stat.Time && stat.Error == nil {
			//stat.Error = fmt.Errorf("no data")
		}
		lines = append(lines, *stat)
	}
	out := cluster.Formatter.FormatLines(lines, lineCount, true)

	//Mark all the host lines that we encountered as having been printed
	for _, stat := range cluster.LastStatLines {
		stat.LastPrinted = stat.Time
	}

	//If appropriate, print a warning message about how the columns relate
	//to the wired tiger storage engine
	if !cluster.lastHadNonMmap {
		cluster.lastHadNonMmap = false
		for _, stat := range cluster.LastStatLines {
			if stat.Error == nil && stat.StorageEngine != "mmapv1" {
				log.Log(log.Always, storageEngineWarning)
				cluster.lastHadNonMmap = true
				break
			}
		}
	}

	fmt.Print(out)
}

//Update() the AsyncCluster
func (cluster *AsyncClusterMonitor) Update(statLine StatLine) {
	cluster.ReportChan <- statLine
}

//The Async implementation of Monitor() starts the goroutines that listen for incoming stat data,
//and dump snapshots at a regular interval.
func (cluster *AsyncClusterMonitor) Monitor(maxRows int, done chan error, sleep time.Duration) {
	receivedData := false
	gotFirstStat := make(chan struct{})
	go func() {
		for {
			newStat := <-cluster.ReportChan
			cluster.updateHostInfo(newStat)
			if !receivedData {
				receivedData = true
				if newStat.Error != nil {
					done <- newStat.Error
					return
				}
				gotFirstStat <- struct{}{}
			}
		}
	}()

	go func() {
		//Wait for the first bit of data to hit the channel before printing
		//anything:
		_ = <-gotFirstStat
		rowCount := 0
		for {
			time.Sleep(sleep)
			cluster.printSnapshot(rowCount, cluster.Discover)
			rowCount++
			if maxRows > 0 && rowCount >= maxRows {
				break
			}
		}
		done <- nil
	}()
}

//Utility constructor for NodeMonitor that copies the same connection settings
//from an instance of ToolOptions, but for a different host name.
func NewNodeMonitor(opts options.ToolOptions, fullHost string, all bool) (*NodeMonitor, error) {
	optsCopy := opts
	host, port := parseHostPort(fullHost)
	optsCopy.Connection = &options.Connection{Host: host, Port: port}
	optsCopy.Direct = true
	sessionProvider, err := db.NewSessionProvider(optsCopy)
	if err != nil {
		return nil, err
	}
	return &NodeMonitor{
		host:            fullHost,
		sessionProvider: sessionProvider,
		LastStatus:      nil,
		LastUpdate:      time.Now(),
		All:             all,
		Err:             nil,
	}, nil
}

//Report collects the stat info for a single node, and sends the result on
//the "out" channel. If it fails, the error is stored in the NodeMonitor Err field.
func (node *NodeMonitor) Poll(discover chan string, all bool, checkShards bool, sampleSecs int64) *StatLine {
	result := &ServerStatus{}
	log.Logf(log.DebugHigh, "getting session on server: %v", node.host)
	s, err := node.sessionProvider.GetSession()
	if err != nil {
		log.Logf(log.DebugLow, "got error getting session to server %v", node.host)
		node.Err = err
		node.LastStatus = nil
		statLine := StatLine{Key: node.host, Host: node.host, Error: err}
		return &statLine
	}
	log.Logf(log.DebugHigh, "got session on server: %v", node.host)

	//The read pref for the session must be set to 'secondary' to enable using
	//the driver with 'direct' connections, which disables the built-in
	//replset discovery mechanism since we do our own node discovery here.
	s.SetMode(mgo.Eventual, true)
	defer s.Close()

	err = s.DB("admin").Run(bson.D{{"serverStatus", 1}, {"recordStats", 0}}, result)
	if err != nil {
		log.Logf(log.DebugLow, "got error calling serverStatus against server %v", node.host)
		result = nil
		statLine := StatLine{Key: node.host, Host: node.host, Error: err}
		return &statLine
	}

	defer func() {
		node.LastStatus = result
	}()

	node.Err = nil
	result.SampleTime = time.Now()

	var statLine *StatLine
	if node.LastStatus != nil && result != nil {
		statLine = NewStatLine(*node.LastStatus, *result, node.host, all, sampleSecs)
	}

	if result.Repl != nil && discover != nil {
		for _, host := range result.Repl.Hosts {
			discover <- host
		}
		for _, host := range result.Repl.Passives {
			discover <- host
		}
	}
	if discover != nil && statLine != nil && statLine.IsMongos && checkShards {
		log.Logf(log.DebugLow, "checking config database to discover shards")
		shardCursor := s.DB("config").C("shards").Find(bson.M{}).Iter()
		shard := ConfigShard{}
		for shardCursor.Next(&shard) {
			shardHosts := strings.Split(shard.Host, ",")
			for _, shardHost := range shardHosts {
				discover <- shardHost
			}
		}
		shardCursor.Close()
	}

	return statLine
}

//Watch spawns a goroutine to continuously collect and process stats for
//a single node on a regular interval. At each interval, the goroutine triggers
//the node's Report function with the 'discover' and 'out' channels.
func (node *NodeMonitor) Watch(sleep time.Duration, discover chan string, cluster ClusterMonitor) {
	go func() {
		cycle := uint64(0)
		for {
			sampleDiff := int64(sleep / time.Second)
			log.Logf(log.DebugHigh, "polling server: %v", node.host)
			statLine := node.Poll(discover, node.All, cycle%10 == 1, sampleDiff)
			if statLine != nil {
				log.Logf(log.DebugHigh, "successfully got statline from host: %v", node.host)
				cluster.Update(*statLine)
			}
			time.Sleep(sleep)
			cycle++
		}
	}()
}

func parseHostPort(fullHostName string) (string, string) {
	if colon := strings.LastIndex(fullHostName, ":"); colon >= 0 {
		return fullHostName[0:colon], fullHostName[colon+1:]
	}
	return fullHostName, "27017"
}

//AddNewNode adds a new host name to be monitored and spawns
//the necessary goroutines to collect data from it.
func (mstat *MongoStat) AddNewNode(fullhost string) error {
	mstat.nodesLock.Lock()
	defer mstat.nodesLock.Unlock()

	if _, hasKey := mstat.Nodes[fullhost]; !hasKey {
		log.Logf(log.DebugLow, "adding new host to monitoring: %v", fullhost)
		//Create a new node monitor for this host.
		node, err := NewNodeMonitor(*mstat.Options, fullhost, mstat.StatOptions.All)
		if err != nil {
			return err
		}
		mstat.Nodes[fullhost] = node
		node.Watch(mstat.SleepInterval, mstat.Discovered, mstat.Cluster)
	}
	return nil
}

//Run is the top-level function that starts the monitoring
//and discovery goroutines
func (mstat *MongoStat) Run() error {

	if mstat.Discovered != nil {
		go func() {
			for {
				newHost := <-mstat.Discovered
				err := mstat.AddNewNode(newHost)
				if err != nil {
					log.Logf(log.Always, "can't add discovered node %v: %v", newHost, err)
				}
			}
		}()
	}

	//Channel to wait
	finished := make(chan error)
	go mstat.Cluster.Monitor(mstat.StatOptions.RowCount, finished, mstat.SleepInterval)

	err := <-finished
	return err

}

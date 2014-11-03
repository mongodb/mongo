package mongostat

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongostat/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"strings"
	"sync"
	"time"
)

//MongoStat is a container for the user-specified options and
//internal cluster state used for running mongostat.
type MongoStat struct {
	// generic mongo tool options
	Options *commonopts.ToolOptions

	// mongostat-specific output options
	StatOptions *options.StatOptions

	//How long to sleep between printing the rows, and polling the server
	SleepInterval time.Duration

	//New nodes can be "discovered" by any other node by sending a hostname
	//on this channel.
	Discovered chan string

	//A map of hostname -> NodeMonitor for all the hosts that
	//are being monitored.
	Nodes map[string]*NodeMonitor

	//ClusterMonitor to manage collecting and printing the stats from all nodes
	Cluster *ClusterMonitor

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

//ClusterMonitor listens on ReportChan for new StatLine objects, and maintains
//an internal map that holds the most recent StatLine for each unique host
//that's been discovered.
type ClusterMonitor struct {
	//Channnel to listen for incoming stat data
	ReportChan chan StatLine

	//Map of hostname -> latest stat data for the host
	LastStatLines map[string]*StatLine

	//Map of hostname -> timestamp of most recent poll event
	LastPollTimes map[string]time.Time

	//Disable printing of column headers
	NoHeaders bool

	//Mutex to protect access to LastStatLines and LastPollTimes
	mapLock sync.Mutex
}

//updateHostInfo updates the internal map with the given StatLine data.
//Safe for concurrent access.
func (cluster *ClusterMonitor) updateHostInfo(stat StatLine) {
	cluster.mapLock.Lock()
	defer cluster.mapLock.Unlock()
	cluster.LastStatLines[stat.Host] = &stat
}

//printSnapshot formats + dumps the current state of all the stats collected
func (cluster *ClusterMonitor) printSnapshot(includeHeaders bool, discover bool) {
	cluster.mapLock.Lock()
	defer cluster.mapLock.Unlock()
	lines := make([]StatLine, 0, len(cluster.LastStatLines))
	for _, stat := range cluster.LastStatLines {
		if stat.LastPrinted == stat.Time && stat.Error == nil {
			stat.Error = fmt.Errorf("no data")
		}
		lines = append(lines, *stat)
	}
	out := FormatLines(lines, includeHeaders, discover)

	//Mark all the host lines that we encountered as having been printed
	for _, stat := range cluster.LastStatLines {
		stat.LastPrinted = stat.Time
	}
	fmt.Print(out)
}

//Monitor starts the goroutines that listen for incoming stat data,
//and dump snapshots at a regular interval.
func (cluster *ClusterMonitor) Monitor(discover bool, maxRows int, done chan error, sleep time.Duration) {
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
		numLines := 0
		for {
			cluster.printSnapshot(numLines == 0 && !cluster.NoHeaders, discover)
			numLines = (numLines + 1) % 10
			rowCount++
			if maxRows > 0 && rowCount >= maxRows {
				break
			}
			time.Sleep(sleep)
		}
		done <- nil
	}()
}

//Utility constructor for NodeMonitor that copies the same connection settings
//from an instance of ToolOptions, but for a different host name.
func NewNodeMonitor(opts commonopts.ToolOptions, fullHost string, all bool) *NodeMonitor {
	optsCopy := opts
	host, port := parseHostPort(fullHost)
	optsCopy.Connection = &commonopts.Connection{Host: host, Port: port}
	optsCopy.Direct = true
	sessionProvider := db.NewSessionProvider(optsCopy)
	return &NodeMonitor{
		host:            fullHost,
		sessionProvider: sessionProvider,
		LastStatus:      nil,
		LastUpdate:      time.Now(),
		All:             all,
		Err:             nil,
	}
}

//Report collects the stat info for a single node, and sends the result on
//the "out" channel. If it fails, the error is stored in the NodeMonitor Err field.
func (node *NodeMonitor) Report(discover chan string, all bool, out chan StatLine, checkShards bool) {
	result := &ServerStatus{}
	s, err := node.sessionProvider.GetSession()
	if err != nil {
		node.Err = err
		node.LastStatus = nil
		statLine := StatLine{Host: node.host, Error: err}
		out <- statLine
		return
	}

	//The read pref for the session must be set to 'secondary' to enable using
	//the driver with 'direct' connections, which disables the built-in
	//replset discovery mechanism since we do our own node discovery here.
	s.SetMode(mgo.Eventual, true)
	defer s.Close()

	err = s.DB("admin").Run(bson.D{{"serverStatus", 1}, {"recordStats", 0}}, result)
	if err != nil {
		result = nil
		statLine := StatLine{Host: node.host, Error: err}

		out <- statLine
		return
	}

	node.Err = nil
	result.SampleTime = time.Now()

	var statLine *StatLine
	if node.LastStatus != nil && result != nil {
		statLine = NewStatLine(*node.LastStatus, *result, node.host, all)
		out <- *statLine
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

	node.LastStatus = result
}

//Watch spawns a goroutine to continuously collect and process stats for
//a single node on a regular interval. At each interval, the goroutine triggers
//the node's Report function with the 'discover' and 'out' channels.
func (node *NodeMonitor) Watch(sleep time.Duration, discover chan string, out chan StatLine) {
	go func() {
		cycle := uint64(0)
		for {
			node.Report(discover, node.All, out, cycle%10 == 1)
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
func (mstat *MongoStat) AddNewNode(fullhost string) {
	mstat.nodesLock.Lock()
	defer mstat.nodesLock.Unlock()

	if _, hasKey := mstat.Nodes[fullhost]; !hasKey {
		//Create a new node monitor for this host.
		node := NewNodeMonitor(*mstat.Options, fullhost, mstat.StatOptions.All)
		mstat.Nodes[fullhost] = node
		node.Watch(mstat.SleepInterval, mstat.Discovered, mstat.Cluster.ReportChan)
	}
}

//Run is the top-level function that starts the monitoring
//and discovery goroutines
func (mstat *MongoStat) Run() error {
	if mstat.Discovered != nil {
		go func() {
			for {
				newHost := <-mstat.Discovered
				mstat.AddNewNode(newHost)
			}
		}()
	}

	//Channel to wait
	finished := make(chan error)
	go mstat.Cluster.Monitor(mstat.Discovered != nil, mstat.StatOptions.RowCount, finished, mstat.SleepInterval)

	err := <-finished
	return err

}

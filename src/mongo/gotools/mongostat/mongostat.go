// Package mongostat provides an overview of the status of a currently running mongod or mongos instance.
package mongostat

import (
	"strings"
	"sync"
	"time"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongostat/stat_consumer"
	"github.com/mongodb/mongo-tools/mongostat/stat_consumer/line"
	"github.com/mongodb/mongo-tools/mongostat/status"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

// MongoStat is a container for the user-specified options and
// internal cluster state used for running mongostat.
type MongoStat struct {
	// Generic mongo tool options.
	Options *options.ToolOptions

	// Mongostat-specific output options.
	StatOptions *StatOptions

	// How long to sleep between printing the rows, and polling the server.
	SleepInterval time.Duration

	// New nodes can be "discovered" by any other node by sending a hostname
	// on this channel.
	Discovered chan string

	// A map of hostname -> NodeMonitor for all the hosts that
	// are being monitored.
	Nodes map[string]*NodeMonitor

	// ClusterMonitor to manage collecting and printing the stats from all nodes.
	Cluster ClusterMonitor

	// Mutex to handle safe concurrent adding to or looping over discovered nodes.
	nodesLock sync.RWMutex
}

// ConfigShard holds a mapping for the format of shard hosts as they
// appear in the config.shards collection.
type ConfigShard struct {
	Id   string `bson:"_id"`
	Host string `bson:"host"`
}

// NodeMonitor contains the connection pool for a single host and collects the
// mongostat data for that host on a regular interval.
type NodeMonitor struct {
	host, alias     string
	sessionProvider *db.SessionProvider

	// The time at which the node monitor last processed an update successfully.
	LastUpdate time.Time

	// The most recent error encountered when collecting stats for this node.
	Err error
}

// SyncClusterMonitor is an implementation of ClusterMonitor that writes output
// synchronized with the timing of when the polling samples are collected.
// Only works with a single host at a time.
type SyncClusterMonitor struct {
	// Channel to listen for incoming stat data
	ReportChan chan *status.ServerStatus

	// Channel to listen for incoming errors
	ErrorChan chan *status.NodeError

	// Creates and consumes StatLines using ServerStatuses
	Consumer *stat_consumer.StatConsumer
}

// ClusterMonitor maintains an internal representation of a cluster's state,
// which can be refreshed with calls to Update(), and dumps output representing
// this internal state on an interval.
type ClusterMonitor interface {
	// Monitor() triggers monitoring and dumping output to begin
	// sleep is the interval to sleep between output dumps.
	// returns an error if it fails, and nil when monitoring ends
	Monitor(sleep time.Duration) error

	// Update signals the ClusterMonitor implementation to refresh its internal
	// state using the data contained in the provided ServerStatus.
	Update(stat *status.ServerStatus, err *status.NodeError)
}

// AsyncClusterMonitor is an implementation of ClusterMonitor that writes output
// gotten from polling samples collected asynchronously from one or more servers.
type AsyncClusterMonitor struct {
	Discover bool

	// Channel to listen for incoming stat data
	ReportChan chan *status.ServerStatus

	// Channel to listen for incoming errors
	ErrorChan chan *status.NodeError

	// Map of hostname -> latest stat data for the host
	LastStatLines map[string]*line.StatLine

	// Mutex to protect access to LastStatLines
	mapLock sync.RWMutex

	// Creates and consumes StatLines using ServerStatuses
	Consumer *stat_consumer.StatConsumer
}

// Update refreshes the internal state of the cluster monitor with the data
// in the StatLine. SyncClusterMonitor's implementation of Update blocks
// until it has written out its state, so that output is always dumped exactly
// once for each poll.
func (cluster *SyncClusterMonitor) Update(stat *status.ServerStatus, err *status.NodeError) {
	if err != nil {
		cluster.ErrorChan <- err
		return
	}
	cluster.ReportChan <- stat
}

// Monitor waits for data on the cluster's report channel. Once new data comes
// in, it formats and then displays it to stdout.
func (cluster *SyncClusterMonitor) Monitor(_ time.Duration) error {
	receivedData := false
	for {
		var statLine *line.StatLine
		var ok bool
		select {
		case stat := <-cluster.ReportChan:
			statLine, ok = cluster.Consumer.Update(stat)
			if !ok {
				continue
			}
		case err := <-cluster.ErrorChan:
			if !receivedData {
				return err
			}
			statLine = &line.StatLine{
				Error:  err,
				Fields: map[string]string{"host": err.Host},
			}
		}
		receivedData = true
		if cluster.Consumer.FormatLines([]*line.StatLine{statLine}) {
			return nil
		}
	}
}

// updateHostInfo updates the internal map with the given StatLine data.
// Safe for concurrent access.
func (cluster *AsyncClusterMonitor) updateHostInfo(stat *line.StatLine) {
	cluster.mapLock.Lock()
	defer cluster.mapLock.Unlock()
	host := stat.Fields["host"]
	cluster.LastStatLines[host] = stat
}

// printSnapshot formats and dumps the current state of all the stats collected.
// returns whether the program should now exit
func (cluster *AsyncClusterMonitor) printSnapshot() bool {
	cluster.mapLock.RLock()
	defer cluster.mapLock.RUnlock()
	lines := make([]*line.StatLine, 0, len(cluster.LastStatLines))
	for _, stat := range cluster.LastStatLines {
		lines = append(lines, stat)
	}
	if len(lines) == 0 {
		return false
	}
	return cluster.Consumer.FormatLines(lines)
}

// Update sends a new StatLine on the cluster's report channel.
func (cluster *AsyncClusterMonitor) Update(stat *status.ServerStatus, err *status.NodeError) {
	if err != nil {
		cluster.ErrorChan <- err
		return
	}
	cluster.ReportChan <- stat
}

// The Async implementation of Monitor starts the goroutines that listen for incoming stat data,
// and dump snapshots at a regular interval.
func (cluster *AsyncClusterMonitor) Monitor(sleep time.Duration) error {
	select {
	case stat := <-cluster.ReportChan:
		cluster.Consumer.Update(stat)
	case err := <-cluster.ErrorChan:
		// error out if the first result is an error
		return err
	}

	go func() {
		for {
			select {
			case stat := <-cluster.ReportChan:
				statLine, ok := cluster.Consumer.Update(stat)
				if ok {
					cluster.updateHostInfo(statLine)
				}
			case err := <-cluster.ErrorChan:
				cluster.updateHostInfo(&line.StatLine{
					Error:  err,
					Fields: map[string]string{"host": err.Host},
				})
			}
		}
	}()

	for range time.Tick(sleep) {
		if cluster.printSnapshot() {
			break
		}
	}
	return nil
}

// NewNodeMonitor copies the same connection settings from an instance of
// ToolOptions, but monitors fullHost.
func NewNodeMonitor(opts options.ToolOptions, fullHost string) (*NodeMonitor, error) {
	optsCopy := opts
	host, port := parseHostPort(fullHost)
	optsCopy.Connection = &options.Connection{
		Host:    host,
		Port:    port,
		Timeout: opts.Timeout,
	}
	optsCopy.Direct = true
	sessionProvider, err := db.NewSessionProvider(optsCopy)
	if err != nil {
		return nil, err
	}
	return &NodeMonitor{
		host:            fullHost,
		sessionProvider: sessionProvider,
		LastUpdate:      time.Now(),
		Err:             nil,
	}, nil
}

// Report collects the stat info for a single node and sends found hostnames on
// the "discover" channel if checkShards is true.
func (node *NodeMonitor) Poll(discover chan string, checkShards bool) (*status.ServerStatus, error) {
	stat := &status.ServerStatus{}
	log.Logvf(log.DebugHigh, "getting session on server: %v", node.host)
	s, err := node.sessionProvider.GetSession()
	if err != nil {
		log.Logvf(log.DebugLow, "got error getting session to server %v", node.host)
		return nil, err
	}
	log.Logvf(log.DebugHigh, "got session on server: %v", node.host)

	// The read pref for the session must be set to 'secondary' to enable using
	// the driver with 'direct' connections, which disables the built-in
	// replset discovery mechanism since we do our own node discovery here.
	s.SetMode(mgo.Eventual, true)

	// Disable the socket timeout - otherwise if db.serverStatus() takes a long time on the server
	// side, the client will close the connection early and report an error.
	s.SetSocketTimeout(0)
	defer s.Close()

	err = s.DB("admin").Run(bson.D{{"serverStatus", 1}, {"recordStats", 0}}, stat)
	if err != nil {
		log.Logvf(log.DebugLow, "got error calling serverStatus against server %v", node.host)
		return nil, err
	}
	statMap := make(map[string]interface{})
	s.DB("admin").Run(bson.D{{"serverStatus", 1}, {"recordStats", 0}}, statMap)
	stat.Flattened = status.Flatten(statMap)

	node.Err = nil
	stat.SampleTime = time.Now()

	if stat.Repl != nil && discover != nil {
		for _, host := range stat.Repl.Hosts {
			discover <- host
		}
		for _, host := range stat.Repl.Passives {
			discover <- host
		}
	}
	node.alias = stat.Host
	stat.Host = node.host
	if discover != nil && stat != nil && status.IsMongos(stat) && checkShards {
		log.Logvf(log.DebugLow, "checking config database to discover shards")
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

	return stat, nil
}

// Watch continuously collects and processes stats for a single node on a
// regular interval. At each interval, it triggers the node's Poll function
// with the 'discover' channel.
func (node *NodeMonitor) Watch(sleep time.Duration, discover chan string, cluster ClusterMonitor) {
	var cycle uint64
	for ticker := time.Tick(sleep); ; <-ticker {
		log.Logvf(log.DebugHigh, "polling server: %v", node.host)
		stat, err := node.Poll(discover, cycle%10 == 0)

		if stat != nil {
			log.Logvf(log.DebugHigh, "successfully got statline from host: %v", node.host)
		}
		var nodeError *status.NodeError
		if err != nil {
			nodeError = status.NewNodeError(node.host, err)
		}
		cluster.Update(stat, nodeError)
		cycle++
	}
}

func parseHostPort(fullHostName string) (string, string) {
	if colon := strings.LastIndex(fullHostName, ":"); colon >= 0 {
		return fullHostName[0:colon], fullHostName[colon+1:]
	}
	return fullHostName, "27017"
}

// AddNewNode adds a new host name to be monitored and spawns the necessary
// goroutine to collect data from it.
func (mstat *MongoStat) AddNewNode(fullhost string) error {
	mstat.nodesLock.Lock()
	defer mstat.nodesLock.Unlock()

	// Remove the 'shardXX/' prefix from the hostname, if applicable
	pieces := strings.Split(fullhost, "/")
	fullhost = pieces[len(pieces)-1]

	if _, hasKey := mstat.Nodes[fullhost]; hasKey {
		return nil
	}
	for _, node := range mstat.Nodes {
		if node.alias == fullhost {
			return nil
		}
	}
	log.Logvf(log.DebugLow, "adding new host to monitoring: %v", fullhost)
	// Create a new node monitor for this host
	node, err := NewNodeMonitor(*mstat.Options, fullhost)
	if err != nil {
		return err
	}
	mstat.Nodes[fullhost] = node
	go node.Watch(mstat.SleepInterval, mstat.Discovered, mstat.Cluster)
	return nil
}

// Run is the top-level function that starts the monitoring
// and discovery goroutines
func (mstat *MongoStat) Run() error {
	if mstat.Discovered != nil {
		go func() {
			for {
				newHost := <-mstat.Discovered
				err := mstat.AddNewNode(newHost)
				if err != nil {
					log.Logvf(log.Always, "can't add discovered node %v: %v", newHost, err)
				}
			}
		}()
	}
	return mstat.Cluster.Monitor(mstat.SleepInterval)
}

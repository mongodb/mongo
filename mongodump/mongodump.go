package mongodump

import (
	"bufio"
	"fmt"
	"github.com/mongodb/mongo-tools/common/auth"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/mongodump/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const (
	ProgressBarLength   = 24
	ProgressBarWaitTime = time.Second * 3

	DumpDefaultPermissions = 0755
)

type MongoDump struct {
	// basic mongo tool options
	ToolOptions *commonopts.ToolOptions

	InputOptions  *options.InputOptions
	OutputOptions *options.OutputOptions

	sessionProvider *db.SessionProvider

	// useful internals that we don't directly expose as options
	manager         *intents.Manager
	useStdout       bool
	query           bson.M
	oplogCollection string
	authVersion     int
	progressManager *progress.Manager
}

// ValidateOptions checks for any incompatible sets of options
func (dump *MongoDump) ValidateOptions() error {
	if err := dump.ToolOptions.Validate(); err != nil {
		return err
	}
	switch {
	case dump.OutputOptions.Out == "-" && dump.ToolOptions.Namespace.Collection == "":
		return fmt.Errorf("can only dump a single collection to stdout")
	case dump.ToolOptions.Namespace.DB == "" && dump.ToolOptions.Namespace.Collection != "":
		return fmt.Errorf("cannot dump a collection without a specified database")
	case dump.InputOptions.Query != "" && dump.ToolOptions.Namespace.Collection == "":
		return fmt.Errorf("cannot dump using a query without a specified collection")
	case dump.OutputOptions.DumpDBUsersAndRoles && dump.ToolOptions.Namespace.DB == "":
		return fmt.Errorf("must specify a database when running with dumpDbUsersAndRoles")
	case dump.OutputOptions.DumpDBUsersAndRoles && dump.ToolOptions.Namespace.Collection != "":
		return fmt.Errorf("cannot specify a collection when running with dumpDbUsersAndRoles") //TODO: why?
	case dump.OutputOptions.Oplog && dump.ToolOptions.Namespace.DB != "":
		return fmt.Errorf("--oplog mode only supported on full dumps")
	case len(dump.OutputOptions.ExcludedCollections) > 0 && dump.ToolOptions.Namespace.Collection != "":
		return fmt.Errorf("--collection is not allowed when --excludeCollection is specified")
	case len(dump.OutputOptions.ExcludedCollectionPrefixes) > 0 && dump.ToolOptions.Namespace.Collection != "":
		return fmt.Errorf("--collection is not allowed when --excludeCollectionsWithPrefix is specified")
	case len(dump.OutputOptions.ExcludedCollections) > 0 && dump.ToolOptions.Namespace.DB == "":
		return fmt.Errorf("--db is required when --excludeCollection is specified")
	case len(dump.OutputOptions.ExcludedCollectionPrefixes) > 0 && dump.ToolOptions.Namespace.DB == "":
		return fmt.Errorf("--db is required when --excludeCollectionsWithPrefix is specified")
	case dump.OutputOptions.Repair && dump.InputOptions.Query != "":
		return fmt.Errorf("cannot run a query with --repair enabled")
	case dump.OutputOptions.JobThreads < 1:
		return fmt.Errorf("number of processing threads must be >= 1")
	}
	return nil
}

func (dump *MongoDump) Init() error {
	err := dump.ValidateOptions()
	if err != nil {
		return fmt.Errorf("Bad Option: %v", err)
	}
	if dump.OutputOptions.Out == "-" {
		dump.useStdout = true
	}
	dump.sessionProvider = db.NewSessionProvider(*dump.ToolOptions)
	dump.manager = intents.NewIntentManager()
	dump.progressManager = progress.NewProgressBarManager(ProgressBarWaitTime)
	return nil
}

// Dump handles some final options checking and executes MongoDump
func (dump *MongoDump) Dump() error {
	err := dump.ValidateOptions()
	// TODO is this duplicated?
	if err != nil {
		return fmt.Errorf("Bad Option: %v", err)
	}

	if dump.InputOptions.Query != "" {
		// parse JSON then convert extended JSON values
		var asJSON interface{}
		err = json.Unmarshal([]byte(dump.InputOptions.Query), &asJSON)
		if err != nil {
			return fmt.Errorf("error parsing query as json: %v", err)
		}
		convertedJSON, err := bsonutil.ConvertJSONValueToBSON(asJSON)
		if err != nil {
			return fmt.Errorf("error converting query to bson: %v", err)
		}
		asMap, ok := convertedJSON.(map[string]interface{})
		if !ok {
			// unlikely to be reached, TODO: think about what could make this happen
			return fmt.Errorf("query is not in proper format")
		}
		dump.query = bson.M(asMap)
	}

	if dump.OutputOptions.DumpDBUsersAndRoles {
		//first make sure this is possible with the connected database
		dump.authVersion, err = auth.GetAuthVersion(dump.sessionProvider)
		if err != nil {
			return fmt.Errorf("error getting auth schema version for dumpDbUsersAndRoles: %v", err)
		}
		log.Logf(log.DebugLow, "using auth schema version %v", dump.authVersion)
		if dump.authVersion < 3 {
			return fmt.Errorf("backing up users and roles is only supported for "+
				"deployments with auth schema versions >= 3, found: %v", dump.authVersion)
		}
	}

	//switch on what kind of execution to do
	switch {
	case dump.ToolOptions.DB == "" && dump.ToolOptions.Collection == "":
		err = dump.CreateAllIntents()
	case dump.ToolOptions.DB != "" && dump.ToolOptions.Collection == "":
		err = dump.CreateIntentsForDatabase(dump.ToolOptions.DB)
	case dump.ToolOptions.DB != "" && dump.ToolOptions.Collection != "":
		err = dump.CreateIntentForCollection(dump.ToolOptions.DB, dump.ToolOptions.Collection)
	}
	if err != nil {
		return err
	}

	var oplogStart bson.MongoTimestamp

	// If oplog capturing is enabled, we first check the most recent
	// oplog entry and save its timestamp, this will let us later
	// copy all oplog entries that occurred while dumping, creating
	// what is effectively a point-in-time snapshot.
	if dump.OutputOptions.Oplog {
		err := dump.determineOplogCollectionName()
		if err != nil {
			return fmt.Errorf("error finding oplog: %v", err)
		}
		log.Logf(log.Info, "getting most recent oplog timestamp")
		oplogStart, err = dump.getOplogStartTime()
		if err != nil {
			return fmt.Errorf("error getting oplog start: %v", err)
		}
	}

	// kick off the progress bar manager and begin dumping intents
	dump.progressManager.Start()
	defer dump.progressManager.Stop()

	if err := dump.DumpIntents(); err != nil {
		return err
	}

	// If we are capturing the oplog, we dump all oplog entries that occurred
	// while dumping the database. Before and after dumping the oplog,
	// we check to see if the oplog has rolled over (i.e. the most recent entry when
	// we started still exist, so we know we haven't lost data)
	if dump.OutputOptions.Oplog {
		log.Logf(log.DebugLow, "checking if oplog entry %v still exists", oplogStart)
		exists, err := dump.checkOplogTimestampExists(oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logf(log.DebugHigh, "oplog entry %v still exists", oplogStart)

		// dump oplog in root of the dump folder
		oplogFilepath := filepath.Join(dump.OutputOptions.Out, "oplog.bson")
		oplogOut, err := os.Create(oplogFilepath)
		if err != nil {
			return fmt.Errorf("error creating bson file `%v`: %v", oplogFilepath, err)
		}

		log.Logf(log.Always, "writing captured oplog to %v", oplogFilepath)
		//TODO encapsulate this logic
		session, err := dump.sessionProvider.GetSession()
		if err != nil {
			return err
		}
		defer session.Close()
		session.SetSocketTimeout(0)
		session.SetPrefetch(1.0) //mimic exhaust cursor
		queryObj := bson.M{"ts": bson.M{"$gt": oplogStart}}
		oplogQuery := session.DB("local").C(dump.oplogCollection).Find(queryObj).LogReplay()
		if err != nil {
			return err
		}
		err = dump.dumpQueryToWriter(
			oplogQuery, &intents.Intent{DB: "local", C: dump.oplogCollection}, oplogOut)
		if err != nil {
			return err
		}

		// check the oplog for a rollover one last time, to avoid a race condition
		// wherein the oplog rolls over in the time after our first check, but before
		// we copy it.
		log.Logf(log.DebugLow, "checking again if oplog entry %v still exists", oplogStart)
		exists, err = dump.checkOplogTimestampExists(oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logf(log.DebugHigh, "oplog entry %v still exists", oplogStart)
	}

	if dump.OutputOptions.DumpDBUsersAndRoles {
		log.Logf(log.Always, "dumping users and roles for %v", dump.ToolOptions.DB)
		if dump.ToolOptions.DB == "admin" {
			log.Logf(log.Always, "skipping users/roles dump, already dumped admin database")
		} else {
			err = dump.DumpUsersAndRolesForDB(dump.ToolOptions.DB)
			if err != nil {
				return fmt.Errorf("error dumping users and roles: %v", err)
			}
		}
	}

	log.Logf(log.Info, "done")

	return err
}

// DumpIntents iterates through the previously-created intents and
// dumps all of the found collections
func (dump *MongoDump) DumpIntents() error {
	resultChan := make(chan error)

	jobs := dump.OutputOptions.JobThreads
	if jobs > 1 {
		dump.manager.Finalize(intents.LongestTaskFirst)
	} else {
		dump.manager.Finalize(intents.Legacy)
	}

	log.Logf(log.Info, "dumping with %v job threads", jobs)

	// start a goroutine for each job thread
	for i := 0; i < jobs; i++ {
		go func(id int) {
			log.Logf(log.DebugHigh, "starting dump routine with id=%v", id)
			for {
				intent := dump.manager.Pop()
				if intent == nil {
					break
				}
				err := dump.DumpIntent(intent)
				if err != nil {
					resultChan <- err
					return
				}
				dump.manager.Finish(intent)
			}
			log.Logf(log.DebugHigh, "ending dump routine with id=%v, no more work to do", id)
			resultChan <- nil
		}(i)
	}

	// wait until all goroutines are done or one of them errors out
	for i := 0; i < jobs; i++ {
		select {
		case err := <-resultChan:
			if err != nil {
				return err
			}
		}
	}

	return nil
}

// DumpCollection dumps the specified database's collection
func (dump *MongoDump) DumpIntent(intent *intents.Intent) error {
	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	session.SetSocketTimeout(0)
	defer session.Close()
	// in mgo, setting prefetch = 1.0 causes the driver to make requests for
	// more results as soon as results are returned. This effectively
	// duplicates the behavior of an exhaust cursor.
	session.SetPrefetch(1.0)

	var findQuery *mgo.Query
	switch {
	case len(dump.query) > 0:
		findQuery = session.DB(intent.DB).C(intent.C).Find(dump.query)
	case dump.InputOptions.TableScan:
		// ---forceTablesScan runs the query without snapshot enabled
		findQuery = session.DB(intent.DB).C(intent.C).Find(nil)
	default:
		findQuery = session.DB(intent.DB).C(intent.C).Find(nil).Snapshot()

	}

	if dump.useStdout {
		log.Logf(log.Always, "writing %v to stdout", intent.Key())
		return dump.dumpQueryToWriter(findQuery, intent, os.Stdout)
	}

	dbFolder := filepath.Join(dump.OutputOptions.Out, intent.DB)
	if err = os.MkdirAll(dbFolder, DumpDefaultPermissions); err != nil {
		return fmt.Errorf("error creating folder `%v` for dump: %v", dbFolder, err)
	}
	outFilepath := filepath.Join(dbFolder, fmt.Sprintf("%v.bson", intent.C))
	out, err := os.Create(outFilepath)
	if err != nil {
		return fmt.Errorf("error creating bson file `%v`: %v", outFilepath, err)
	}
	defer out.Close()

	if !dump.OutputOptions.Repair {
		log.Logf(log.Always, "writing %v to %v", intent.Key(), outFilepath)
		if err = dump.dumpQueryToWriter(findQuery, intent, out); err != nil {
			return err
		}
	} else {
		// handle repairs as a special case, since we cannot count them
		log.Logf(log.Always, "writing repair of %v to %v", intent.Key(), outFilepath)
		repairIter := session.DB(intent.DB).C(intent.C).Repair()
		repairCounter := 0
		if err := dump.dumpIterToWriter(repairIter, out, &repairCounter); err != nil {
			if strings.Index(err.Error(), "no such cmd: repairCursor") > 0 {
				// return a more helpful error message for early server versions
				return fmt.Errorf(
					"error: --repair flag cannot be used on mongodb versions before 2.7.8.")
			}
			return fmt.Errorf("repair error: %v", err)
		}
		log.Logf(log.Always,
			"\trepair cursor found %v documents in %v", repairCounter, intent.Key())
	}

	// don't dump metatdata for SystemIndexes collection
	if intent.IsSystemIndexes() {
		return nil
	}

	metadataFilepath := filepath.Join(dbFolder, fmt.Sprintf("%v.metadata.json", intent.C))
	metaOut, err := os.Create(metadataFilepath)
	if err != nil {
		return fmt.Errorf("error creating metadata.json file `%v`: %v", outFilepath, err)
	}
	defer metaOut.Close()

	log.Logf(log.Always, "writing %v metadata to %v", intent.Key(), metadataFilepath)
	if err = dump.dumpMetadataToWriter(intent.DB, intent.C, metaOut); err != nil {
		return err
	}

	log.Logf(log.Always, "done dumping %v", intent.Key())
	return nil
}

// dumpQueryToWriter takes an mgo Query, its intent, and a writer, performs the query,
// and writes the raw bson results to the writer.
func (dump *MongoDump) dumpQueryToWriter(
	query *mgo.Query, intent *intents.Intent, writer io.Writer) (err error) {

	dumpCounter := 0

	total, err := query.Count()
	if err != nil {
		return fmt.Errorf("error reading from db: %v", err)
	}
	log.Logf(log.Info, "\t%v documents", total)

	bar := &progress.ProgressBar{
		Name:       intent.Key(),
		Max:        total,
		CounterPtr: &dumpCounter,
		Writer:     log.Writer(0),
		BarLength:  ProgressBarLength,
	}
	dump.progressManager.Attach(bar)
	defer dump.progressManager.Detach(bar)

	// We run the result iteration in its own goroutine,
	// this allows disk i/o to not block reads from the db,
	// which gives a slight speedup on benchmarks
	iter := query.Iter()
	return dump.dumpIterToWriter(iter, writer, &dumpCounter)
}

// dumpIterToWriter takes an mgo iterator, a writer, and a pointer to
// a counter, and dumps the iterator's contents to the writer.
func (dump *MongoDump) dumpIterToWriter(
	iter *mgo.Iter, writer io.Writer, counterPtr *int) error {

	buffChan := make(chan []byte)
	go func() {
		for {
			raw := &bson.Raw{}
			if err := iter.Err(); err != nil {
				log.Logf(log.Always, "error reading from db: %v", err)
			}
			next := iter.Next(raw)

			if !next {
				close(buffChan)
				return
			}

			//TODO use buffer pool?
			nextCopy := make([]byte, len(raw.Data))
			copy(nextCopy, raw.Data)

			buffChan <- nextCopy
		}
	}()

	// wrap writer in buffer to reduce load on disk
	// TODO extensive optimization on buffer size
	w := bufio.NewWriterSize(writer, 1024*32)

	// while there are still results in the database,
	// grab results from the goroutine and write them to filesystem
	for {
		buff, alive := <-buffChan
		if !alive {
			if iter.Err() != nil {
				return fmt.Errorf("error reading collection: %v", iter.Err())
			}
			break
		}
		_, err := w.Write(buff)
		if err != nil {
			return fmt.Errorf("error writing to file: %v", err)
		}
		*counterPtr++
	}
	err := w.Flush()
	if err != nil {
		return fmt.Errorf("error flushing file writer: %v", err)
	}
	return nil
}

// DumpUsersAndRolesForDB queries and dumps the users and roles tied
// to the given the db. Only works with schema version >= 3
func (dump *MongoDump) DumpUsersAndRolesForDB(db string) error {
	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	session.SetSocketTimeout(0)
	defer session.Close()

	dbQuery := bson.M{"db": db}
	outDir := filepath.Join(dump.OutputOptions.Out, db)

	usersFile, err := os.Create(filepath.Join(outDir, "$admin.system.users.bson"))
	if err != nil {
		return fmt.Errorf("error creating file for db users: %v", err)
	}

	usersQuery := session.DB("admin").C("system.users").Find(dbQuery)
	err = dump.dumpQueryToWriter(
		usersQuery, &intents.Intent{DB: "system", C: "users"}, usersFile)
	if err != nil {
		return fmt.Errorf("error dumping db users: %v", err)
	}

	rolesFile, err := os.Create(filepath.Join(outDir, "$admin.system.roles.bson"))
	if err != nil {
		return fmt.Errorf("error creating file for db roles: %v", err)
	}

	rolesQuery := session.DB("admin").C("system.roles").Find(dbQuery)
	err = dump.dumpQueryToWriter(
		rolesQuery, &intents.Intent{DB: "system", C: "roles"}, rolesFile)
	if err != nil {
		return fmt.Errorf("error dumping db roles: %v", err)
	}

	return nil
}

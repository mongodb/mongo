// Package mongodump creates BSON data from the contents of a MongoDB instance.
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
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"path/filepath"
	"time"
)

const (
	progressBarLength   = 24
	progressBarWaitTime = time.Second * 3

	defaultPermissions = 0755
)

// MongoDump is a container for the user-specified options and
// internal state used for running mongodump.
type MongoDump struct {
	// basic mongo tool options
	ToolOptions   *options.ToolOptions
	InputOptions  *InputOptions
	OutputOptions *OutputOptions

	// useful internals that we don't directly expose as options
	sessionProvider *db.SessionProvider
	manager         *intents.Manager
	useStdout       bool
	query           bson.M
	oplogCollection string
	oplogStart      bson.MongoTimestamp
	isMongos        bool
	authVersion     int
	progressManager *progress.Manager
}

// ValidateOptions checks for any incompatible sets of options.
func (dump *MongoDump) ValidateOptions() error {
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
		return fmt.Errorf("cannot specify a collection when running with dumpDbUsersAndRoles")
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
	}
	return nil
}

// Init performs preliminary setup operations for MongoDump.
func (dump *MongoDump) Init() error {
	err := dump.ValidateOptions()
	if err != nil {
		return fmt.Errorf("bad option: %v", err)
	}
	if dump.OutputOptions.Out == "-" {
		dump.useStdout = true
	}
	dump.sessionProvider, err = db.NewSessionProvider(*dump.ToolOptions)
	if err != nil {
		return fmt.Errorf("can't create session: %v", err)
	}

	// allow secondary reads for the isMongos check
	dump.sessionProvider.SetFlags(db.Monotonic)
	dump.isMongos, err = dump.sessionProvider.IsMongos()
	if err != nil {
		return err
	}

	// ensure we allow secondary reads on mongods and disable TCP timeouts
	flags := db.DisableSocketTimeout
	if dump.isMongos {
		log.Logf(log.Info, "connecting to mongos; secondary reads disabled")
	} else {
		flags |= db.Monotonic
	}
	dump.sessionProvider.SetFlags(flags)

	// return a helpful error message for mongos --repair
	if dump.OutputOptions.Repair && dump.isMongos {
		return fmt.Errorf("--repair flag cannot be used on a mongos")
	}

	dump.manager = intents.NewIntentManager()
	dump.progressManager = progress.NewProgressBarManager(log.Writer(0), progressBarWaitTime)
	return nil
}

// Dump handles some final options checking and executes MongoDump.
func (dump *MongoDump) Dump() error {
	var err error
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
			// unlikely to be reached
			return fmt.Errorf("query is not in proper format")
		}
		dump.query = bson.M(asMap)
	}

	if dump.OutputOptions.DumpDBUsersAndRoles {
		// first make sure this is possible with the connected database
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

	// switch on what kind of execution to do
	switch {
	case dump.ToolOptions.DB == "" && dump.ToolOptions.Collection == "":
		err = dump.CreateAllIntents()
	case dump.ToolOptions.DB != "" && dump.ToolOptions.Collection == "":
		err = dump.CreateIntentsForDatabase(dump.ToolOptions.DB)
	case dump.ToolOptions.DB != "" && dump.ToolOptions.Collection != "":
		err = dump.CreateCollectionIntent(dump.ToolOptions.DB, dump.ToolOptions.Collection)
	}
	if err != nil {
		return err
	}

	// verify we can use repair cursors
	if dump.OutputOptions.Repair {
		log.Log(log.DebugLow, "verifying that the connected server supports repairCursor")
		if dump.isMongos {
			return fmt.Errorf("cannot use --repair on mongos")
		}
		exampleIntent := dump.manager.Peek()
		if exampleIntent != nil {
			supported, err := dump.sessionProvider.SupportsRepairCursor(
				exampleIntent.DB, exampleIntent.C)
			if !supported {
				return err // no extra context needed
			}
		}
	}

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
		dump.oplogStart, err = dump.getOplogStartTime()
		if err != nil {
			return fmt.Errorf("error getting oplog start: %v", err)
		}
	}

	// kick off the progress bar manager and begin dumping intents
	dump.progressManager.Start()
	defer dump.progressManager.Stop()

	// dump all queued collections
	if err := dump.DumpIntents(); err != nil {
		return err
	}

	// If we are capturing the oplog, we dump all oplog entries that occurred
	// while dumping the database. Before and after dumping the oplog,
	// we check to see if the oplog has rolled over (i.e. the most recent entry when
	// we started still exist, so we know we haven't lost data)
	if dump.OutputOptions.Oplog {
		log.Logf(log.DebugLow, "checking if oplog entry %v still exists", dump.oplogStart)
		exists, err := dump.checkOplogTimestampExists(dump.oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logf(log.DebugHigh, "oplog entry %v still exists", dump.oplogStart)

		// dump oplog in root of the dump folder
		oplogFilepath := filepath.Join(dump.OutputOptions.Out, "oplog.bson")
		oplogOut, err := os.Create(oplogFilepath)
		if err != nil {
			return fmt.Errorf("error creating bson file `%v`: %v", oplogFilepath, err)
		}
		log.Logf(log.Always, "writing captured oplog to %v", oplogFilepath)
		err = dump.DumpOplogAfterTimestamp(dump.oplogStart, oplogOut)
		if err != nil {
			return fmt.Errorf("error dumping oplog: %v", err)
		}

		// check the oplog for a rollover one last time, to avoid a race condition
		// wherein the oplog rolls over in the time after our first check, but before
		// we copy it.
		log.Logf(log.DebugLow, "checking again if oplog entry %v still exists", dump.oplogStart)
		exists, err = dump.checkOplogTimestampExists(dump.oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logf(log.DebugHigh, "oplog entry %v still exists", dump.oplogStart)
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
// dumps all of the found collections.
func (dump *MongoDump) DumpIntents() error {
	resultChan := make(chan error)

	var jobs int
	if dump.ToolOptions != nil && dump.ToolOptions.HiddenOptions != nil {
		jobs = dump.ToolOptions.HiddenOptions.MaxProcs
	}
	jobs = util.MaxInt(jobs, 1)
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
					log.Logf(log.DebugHigh, "ending dump routine with id=%v, no more work to do", id)
					resultChan <- nil
					return
				}
				err := dump.DumpIntent(intent)
				if err != nil {
					resultChan <- err
					return
				}
				dump.manager.Finish(intent)
			}
		}(i)
	}

	// wait until all goroutines are done or one of them errors out
	for i := 0; i < jobs; i++ {
		if err := <-resultChan; err != nil {
			return err
		}
	}

	return nil
}

// DumpCollection dumps the specified database's collection.
func (dump *MongoDump) DumpIntent(intent *intents.Intent) error {
	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return err
	}
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

	var dumpCount int64
	if dump.useStdout {
		log.Logf(log.Always, "writing %v to stdout", intent.Namespace())
		dumpCount, err = dump.dumpQueryToWriter(findQuery, intent, os.Stdout)
		if err == nil {
			// on success, print the document count
			log.Logf(log.Always, "dumped %v %v", dumpCount, docPlural(dumpCount))
		}
		return err
	}

	dbFolder := filepath.Join(dump.OutputOptions.Out, intent.DB)
	if err = os.MkdirAll(dbFolder, defaultPermissions); err != nil {
		return fmt.Errorf("error creating folder `%v` for dump: %v", dbFolder, err)
	}
	outFilepath := filepath.Join(dbFolder, fmt.Sprintf("%v.bson", intent.C))
	out, err := os.Create(outFilepath)
	if err != nil {
		return fmt.Errorf("error creating bson file `%v`: %v", outFilepath, err)
	}
	defer out.Close()

	if !dump.OutputOptions.Repair {
		log.Logf(log.Always, "writing %v to %v", intent.Namespace(), outFilepath)
		if dumpCount, err = dump.dumpQueryToWriter(findQuery, intent, out); err != nil {
			return err
		}
	} else {
		// handle repairs as a special case, since we cannot count them
		log.Logf(log.Always, "writing repair of %v to %v", intent.Namespace(), outFilepath)
		repairIter := session.DB(intent.DB).C(intent.C).Repair()
		repairCounter := progress.NewCounter(1) // this counter is ignored
		if err := dump.dumpIterToWriter(repairIter, out, repairCounter); err != nil {
			return fmt.Errorf("repair error: %v", err)
		}
		_, repairCount := repairCounter.Progress()
		log.Logf(log.Always, "\trepair cursor found %v %v in %v",
			repairCount, docPlural(repairCount), intent.Namespace())
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

	log.Logf(log.Always, "writing %v metadata to %v", intent.Namespace(), metadataFilepath)
	if err = dump.dumpMetadataToWriter(intent, metaOut); err != nil {
		return err
	}

	log.Logf(log.Always, "done dumping %v (%v %v)", intent.Namespace(), dumpCount, docPlural(dumpCount))
	return nil
}

// dumpQueryToWriter takes an mgo Query, its intent, and a writer, performs the query,
// and writes the raw bson results to the writer. Returns a final count of documents
// dumped, and any errors that occured.
func (dump *MongoDump) dumpQueryToWriter(
	query *mgo.Query, intent *intents.Intent, writer io.Writer) (int64, error) {

	total, err := query.Count()
	if err != nil {
		return int64(0), fmt.Errorf("error reading from db: %v", err)
	}
	log.Logf(log.Info, "\tcounted %v %v in %v", total, docPlural(int64(total)), intent.Namespace())

	dumpProgressor := progress.NewCounter(int64(total))
	bar := &progress.Bar{
		Name:      intent.Namespace(),
		Watching:  dumpProgressor,
		BarLength: progressBarLength,
	}
	dump.progressManager.Attach(bar)
	defer dump.progressManager.Detach(bar)

	err = dump.dumpIterToWriter(query.Iter(), writer, dumpProgressor)
	_, dumpCount := dumpProgressor.Progress()
	return dumpCount, err
}

// dumpIterToWriter takes an mgo iterator, a writer, and a pointer to
// a counter, and dumps the iterator's contents to the writer.
func (dump *MongoDump) dumpIterToWriter(
	iter *mgo.Iter, writer io.Writer, progressCount progress.Updateable) error {

	// We run the result iteration in its own goroutine,
	// this allows disk i/o to not block reads from the db,
	// which gives a slight speedup on benchmarks
	buffChan := make(chan []byte)
	go func() {
		for {
			raw := &bson.Raw{}
			next := iter.Next(raw)
			if !next {
				// we check the iterator for errors below
				close(buffChan)
				return
			}

			nextCopy := make([]byte, len(raw.Data))
			copy(nextCopy, raw.Data)

			buffChan <- nextCopy
		}
	}()

	// wrap writer in buffer to reduce load on disk
	w := bufio.NewWriterSize(writer, 32*1024)

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
		progressCount.Inc(1)
	}

	// flush all remaining disk writes then exit
	err := w.Flush()
	if err != nil {
		return fmt.Errorf("error flushing file writer: %v", err)
	}
	return nil
}

// DumpUsersAndRolesForDB queries and dumps the users and roles tied to the given
// database. Only works with an authentication schema version >= 3.
func (dump *MongoDump) DumpUsersAndRolesForDB(db string) error {
	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	dbQuery := bson.M{"db": db}
	outDir := filepath.Join(dump.OutputOptions.Out, db)

	usersFile, err := os.Create(filepath.Join(outDir, "$admin.system.users.bson"))
	if err != nil {
		return fmt.Errorf("error creating file for db users: %v", err)
	}
	usersQuery := session.DB("admin").C("system.users").Find(dbQuery)
	_, err = dump.dumpQueryToWriter(usersQuery, &intents.Intent{DB: "system", C: "users"}, usersFile)
	if err != nil {
		return fmt.Errorf("error dumping db users: %v", err)
	}

	rolesFile, err := os.Create(filepath.Join(outDir, "$admin.system.roles.bson"))
	if err != nil {
		return fmt.Errorf("error creating file for db roles: %v", err)
	}
	rolesQuery := session.DB("admin").C("system.roles").Find(dbQuery)
	_, err = dump.dumpQueryToWriter(
		rolesQuery, &intents.Intent{DB: "system", C: "roles"}, rolesFile)
	if err != nil {
		return fmt.Errorf("error dumping db roles: %v", err)
	}

	versionFile, err := os.Create(filepath.Join(outDir, "$admin.system.version.bson"))
	if err != nil {
		return fmt.Errorf("error creating file for db auth version: %v", err)
	}
	versionQuery := session.DB("admin").C("system.version").Find(nil)
	_, err = dump.dumpQueryToWriter(versionQuery, &intents.Intent{DB: "system", C: "version"}, versionFile)
	if err != nil {
		return fmt.Errorf("error dumping db auth version: %v", err)
	}

	return nil
}

// docPlural returns "document" or "documents" depending on the
// count of documents passed in.
func docPlural(count int64) string {
	return util.Pluralize(int(count), "document", "documents")
}

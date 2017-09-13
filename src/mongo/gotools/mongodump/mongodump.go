// Package mongodump creates BSON data from the contents of a MongoDB instance.
package mongodump

import (
	"github.com/mongodb/mongo-tools/common/archive"
	"github.com/mongodb/mongo-tools/common/auth"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/failpoint"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"

	"bufio"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const defaultPermissions = 0755

// MongoDump is a container for the user-specified options and
// internal state used for running mongodump.
type MongoDump struct {
	// basic mongo tool options
	ToolOptions   *options.ToolOptions
	InputOptions  *InputOptions
	OutputOptions *OutputOptions

	// Skip dumping users and roles, regardless of namespace, when true.
	SkipUsersAndRoles bool

	ProgressManager progress.Manager

	// useful internals that we don't directly expose as options
	SessionProvider *db.SessionProvider
	manager         *intents.Manager
	query           bson.M
	oplogCollection string
	oplogStart      bson.MongoTimestamp
	isMongos        bool
	authVersion     int
	archive         *archive.Writer
	// shutdownIntentsNotifier is provided to the multiplexer
	// as well as the signal handler, and allows them to notify
	// the intent dumpers that they should shutdown
	shutdownIntentsNotifier *notifier
	// Writer to take care of BSON output when not writing to the local filesystem.
	// This is initialized to os.Stdout if unset.
	OutputWriter io.Writer
	readPrefMode mgo.Mode
	readPrefTags []bson.D
}

type notifier struct {
	notified chan struct{}
	once     sync.Once
}

func (n *notifier) Notify() { n.once.Do(func() { close(n.notified) }) }

func newNotifier() *notifier { return &notifier{notified: make(chan struct{})} }

// ValidateOptions checks for any incompatible sets of options.
func (dump *MongoDump) ValidateOptions() error {
	switch {
	case dump.OutputOptions.Out == "-" && dump.ToolOptions.Namespace.Collection == "":
		return fmt.Errorf("can only dump a single collection to stdout")
	case dump.ToolOptions.Namespace.DB == "" && dump.ToolOptions.Namespace.Collection != "":
		return fmt.Errorf("cannot dump a collection without a specified database")
	case dump.InputOptions.Query != "" && dump.ToolOptions.Namespace.Collection == "":
		return fmt.Errorf("cannot dump using a query without a specified collection")
	case dump.InputOptions.QueryFile != "" && dump.ToolOptions.Namespace.Collection == "":
		return fmt.Errorf("cannot dump using a queryFile without a specified collection")
	case dump.InputOptions.Query != "" && dump.InputOptions.QueryFile != "":
		return fmt.Errorf("either query or queryFile can be specified as a query option, not both")
	case dump.InputOptions.Query != "" && dump.InputOptions.TableScan:
		return fmt.Errorf("cannot use --forceTableScan when specifying --query")
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
	case dump.OutputOptions.Repair && dump.InputOptions.QueryFile != "":
		return fmt.Errorf("cannot run a queryFile with --repair enabled")
	case dump.OutputOptions.Out != "" && dump.OutputOptions.Archive != "":
		return fmt.Errorf("--out not allowed when --archive is specified")
	case dump.OutputOptions.Out == "-" && dump.OutputOptions.Gzip:
		return fmt.Errorf("compression can't be used when dumping a single collection to standard output")
	case dump.OutputOptions.NumParallelCollections <= 0:
		return fmt.Errorf("numParallelCollections must be positive")
	}
	return nil
}

// Init performs preliminary setup operations for MongoDump.
func (dump *MongoDump) Init() error {
	err := dump.ValidateOptions()
	if err != nil {
		return fmt.Errorf("bad option: %v", err)
	}
	if dump.OutputWriter == nil {
		dump.OutputWriter = os.Stdout
	}
	dump.SessionProvider, err = db.NewSessionProvider(*dump.ToolOptions)
	if err != nil {
		return fmt.Errorf("can't create session: %v", err)
	}

	// temporarily allow secondary reads for the isMongos check
	dump.SessionProvider.SetReadPreference(mgo.Nearest)
	dump.isMongos, err = dump.SessionProvider.IsMongos()
	if err != nil {
		return err
	}

	if dump.isMongos && dump.OutputOptions.Oplog {
		return fmt.Errorf("can't use --oplog option when dumping from a mongos")
	}

	var mode mgo.Mode
	if dump.ToolOptions.ReplicaSetName != "" || dump.isMongos {
		mode = mgo.Primary
	} else {
		mode = mgo.Nearest
	}
	var tags bson.D

	if dump.InputOptions.ReadPreference != "" {
		mode, tags, err = db.ParseReadPreference(dump.InputOptions.ReadPreference)
		if err != nil {
			return fmt.Errorf("error parsing --readPreference : %v", err)
		}
		if len(tags) > 0 {
			dump.SessionProvider.SetTags(tags)
		}
	}

	// warn if we are trying to dump from a secondary in a sharded cluster
	if dump.isMongos && mode != mgo.Primary {
		log.Logvf(log.Always, db.WarningNonPrimaryMongosConnection)
	}

	dump.SessionProvider.SetReadPreference(mode)
	dump.SessionProvider.SetTags(tags)
	dump.SessionProvider.SetFlags(db.DisableSocketTimeout)

	// return a helpful error message for mongos --repair
	if dump.OutputOptions.Repair && dump.isMongos {
		return fmt.Errorf("--repair flag cannot be used on a mongos")
	}

	dump.manager = intents.NewIntentManager()
	return nil
}

// Dump handles some final options checking and executes MongoDump.
func (dump *MongoDump) Dump() (err error) {
	defer dump.SessionProvider.Close()

	dump.shutdownIntentsNotifier = newNotifier()

	if dump.InputOptions.HasQuery() {
		// parse JSON then convert extended JSON values
		var asJSON interface{}
		content, err := dump.InputOptions.GetQuery()
		if err != nil {
			return err
		}
		err = json.Unmarshal(content, &asJSON)
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

	if !dump.SkipUsersAndRoles && dump.OutputOptions.DumpDBUsersAndRoles {
		// first make sure this is possible with the connected database
		dump.authVersion, err = auth.GetAuthVersion(dump.SessionProvider)
		if err == nil {
			err = auth.VerifySystemAuthVersion(dump.SessionProvider)
		}
		if err != nil {
			return fmt.Errorf("error getting auth schema version for dumpDbUsersAndRoles: %v", err)
		}
		log.Logvf(log.DebugLow, "using auth schema version %v", dump.authVersion)
		if dump.authVersion < 3 {
			return fmt.Errorf("backing up users and roles is only supported for "+
				"deployments with auth schema versions >= 3, found: %v", dump.authVersion)
		}
	}

	if dump.OutputOptions.Archive != "" {
		//getArchiveOut gives us a WriteCloser to which we should write the archive
		var archiveOut io.WriteCloser
		archiveOut, err = dump.getArchiveOut()
		if err != nil {
			return err
		}
		dump.archive = &archive.Writer{
			// The archive.Writer needs its own copy of archiveOut because things
			// like the prelude are not written by the multiplexer.
			Out: archiveOut,
			Mux: archive.NewMultiplexer(archiveOut, dump.shutdownIntentsNotifier),
		}
		go dump.archive.Mux.Run()
		defer func() {
			// The Mux runs until its Control is closed
			close(dump.archive.Mux.Control)
			muxErr := <-dump.archive.Mux.Completed
			archiveOut.Close()
			if muxErr != nil {
				if err != nil {
					err = fmt.Errorf("archive writer: %v / %v", err, muxErr)
				} else {
					err = fmt.Errorf("archive writer: %v", muxErr)
				}
				log.Logvf(log.DebugLow, "%v", err)
			} else {
				log.Logvf(log.DebugLow, "mux completed successfully")
			}
		}()
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

	if dump.OutputOptions.Oplog {
		err = dump.CreateOplogIntents()
		if err != nil {
			return err
		}
	}

	if !dump.SkipUsersAndRoles && dump.OutputOptions.DumpDBUsersAndRoles && dump.ToolOptions.DB != "admin" {
		err = dump.CreateUsersRolesVersionIntentsForDB(dump.ToolOptions.DB)
		if err != nil {
			return err
		}
	}

	// verify we can use repair cursors
	if dump.OutputOptions.Repair {
		log.Logv(log.DebugLow, "verifying that the connected server supports repairCursor")
		if dump.isMongos {
			return fmt.Errorf("cannot use --repair on mongos")
		}
		exampleIntent := dump.manager.Peek()
		if exampleIntent != nil {
			supported, err := dump.SessionProvider.SupportsRepairCursor(
				exampleIntent.DB, exampleIntent.C)
			if !supported {
				return err // no extra context needed
			}
		}
	}

	// IO Phase I
	// metadata, users, roles, and versions

	// TODO, either remove this debug or improve the language
	log.Logvf(log.DebugHigh, "dump phase I: metadata, indexes, users, roles, version")

	err = dump.DumpMetadata()
	if err != nil {
		return fmt.Errorf("error dumping metadata: %v", err)
	}

	if dump.OutputOptions.Archive != "" {
		session, err := dump.SessionProvider.GetSession()
		if err != nil {
			return err
		}
		defer session.Close()
		buildInfo, err := session.BuildInfo()
		var serverVersion string
		if err != nil {
			log.Logvf(log.Always, "warning, couldn't get version information from server: %v", err)
			serverVersion = "unknown"
		} else {
			serverVersion = buildInfo.Version
		}
		dump.archive.Prelude, err = archive.NewPrelude(dump.manager, dump.OutputOptions.NumParallelCollections, serverVersion)
		if err != nil {
			return fmt.Errorf("creating archive prelude: %v", err)
		}
		err = dump.archive.Prelude.Write(dump.archive.Out)
		if err != nil {
			return fmt.Errorf("error writing metadata into archive: %v", err)
		}
	}

	err = dump.DumpSystemIndexes()
	if err != nil {
		return fmt.Errorf("error dumping system indexes: %v", err)
	}

	if !dump.SkipUsersAndRoles {
		if dump.ToolOptions.DB == "admin" || dump.ToolOptions.DB == "" {
			err = dump.DumpUsersAndRoles()
			if err != nil {
				return fmt.Errorf("error dumping users and roles: %v", err)
			}
		}
		if dump.OutputOptions.DumpDBUsersAndRoles {
			log.Logvf(log.Always, "dumping users and roles for %v", dump.ToolOptions.DB)
			if dump.ToolOptions.DB == "admin" {
				log.Logvf(log.Always, "skipping users/roles dump, already dumped admin database")
			} else {
				err = dump.DumpUsersAndRolesForDB(dump.ToolOptions.DB)
				if err != nil {
					return fmt.Errorf("error dumping users and roles: %v", err)
				}
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
		log.Logvf(log.Info, "getting most recent oplog timestamp")
		dump.oplogStart, err = dump.getOplogStartTime()
		if err != nil {
			return fmt.Errorf("error getting oplog start: %v", err)
		}
	}

	if failpoint.Enabled(failpoint.PauseBeforeDumping) {
		time.Sleep(15 * time.Second)
	}

	// IO Phase II
	// regular collections

	// TODO, either remove this debug or improve the language
	log.Logvf(log.DebugHigh, "dump phase II: regular collections")

	// begin dumping intents
	if err := dump.DumpIntents(); err != nil {
		return err
	}

	// IO Phase III
	// oplog

	// TODO, either remove this debug or improve the language
	log.Logvf(log.DebugLow, "dump phase III: the oplog")

	// If we are capturing the oplog, we dump all oplog entries that occurred
	// while dumping the database. Before and after dumping the oplog,
	// we check to see if the oplog has rolled over (i.e. the most recent entry when
	// we started still exist, so we know we haven't lost data)
	if dump.OutputOptions.Oplog {
		log.Logvf(log.DebugLow, "checking if oplog entry %v still exists", dump.oplogStart)
		exists, err := dump.checkOplogTimestampExists(dump.oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logvf(log.DebugHigh, "oplog entry %v still exists", dump.oplogStart)

		log.Logvf(log.Always, "writing captured oplog to %v", dump.manager.Oplog().Location)
		err = dump.DumpOplogAfterTimestamp(dump.oplogStart)
		if err != nil {
			return fmt.Errorf("error dumping oplog: %v", err)
		}

		// check the oplog for a rollover one last time, to avoid a race condition
		// wherein the oplog rolls over in the time after our first check, but before
		// we copy it.
		log.Logvf(log.DebugLow, "checking again if oplog entry %v still exists", dump.oplogStart)
		exists, err = dump.checkOplogTimestampExists(dump.oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logvf(log.DebugHigh, "oplog entry %v still exists", dump.oplogStart)
	}

	log.Logvf(log.DebugLow, "finishing dump")

	return err
}

type resettableOutputBuffer interface {
	io.Writer
	Close() error
	Reset(io.Writer)
}

type closableBufioWriter struct {
	*bufio.Writer
}

func (w closableBufioWriter) Close() error {
	return w.Flush()
}

func (dump *MongoDump) getResettableOutputBuffer() resettableOutputBuffer {
	if dump.OutputOptions.Archive != "" {
		return nil
	} else if dump.OutputOptions.Gzip {
		return gzip.NewWriter(nil)
	}
	return &closableBufioWriter{bufio.NewWriter(nil)}
}

// DumpIntents iterates through the previously-created intents and
// dumps all of the found collections.
func (dump *MongoDump) DumpIntents() error {
	resultChan := make(chan error)

	jobs := dump.OutputOptions.NumParallelCollections
	if numIntents := len(dump.manager.Intents()); jobs > numIntents {
		jobs = numIntents
	}

	if jobs > 1 {
		dump.manager.Finalize(intents.LongestTaskFirst)
	} else {
		dump.manager.Finalize(intents.Legacy)
	}

	log.Logvf(log.Info, "dumping up to %v collections in parallel", jobs)

	// start a goroutine for each job thread
	for i := 0; i < jobs; i++ {
		go func(id int) {
			buffer := dump.getResettableOutputBuffer()
			log.Logvf(log.DebugHigh, "starting dump routine with id=%v", id)
			for {
				intent := dump.manager.Pop()
				if intent == nil {
					log.Logvf(log.DebugHigh, "ending dump routine with id=%v, no more work to do", id)
					resultChan <- nil
					return
				}
				if intent.BSONFile != nil {
					err := dump.DumpIntent(intent, buffer)
					if err != nil {
						resultChan <- err
						return
					}
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

// DumpIntent dumps the specified database's collection.
func (dump *MongoDump) DumpIntent(intent *intents.Intent, buffer resettableOutputBuffer) error {
	session, err := dump.SessionProvider.GetSession()
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
	case dump.OutputOptions.ViewsAsCollections:
		// views have an implied aggregation which does not support snapshot
		fallthrough
	case dump.InputOptions.TableScan:
		// ---forceTablesScan runs the query without snapshot enabled
		findQuery = session.DB(intent.DB).C(intent.C).Find(nil)
	default:
		findQuery = session.DB(intent.DB).C(intent.C).Find(nil).Snapshot()
	}

	var dumpCount int64

	if dump.OutputOptions.Out == "-" {
		log.Logvf(log.Always, "writing %v to stdout", intent.Namespace())
		dumpCount, err = dump.dumpQueryToIntent(findQuery, intent, buffer)
		if err == nil {
			// on success, print the document count
			log.Logvf(log.Always, "dumped %v %v", dumpCount, docPlural(dumpCount))
		}
		return err
	}

	// set where the intent will be written to
	if dump.OutputOptions.Archive != "" {
		if dump.OutputOptions.Archive == "-" {
			intent.Location = "archive on stdout"
		} else {
			intent.Location = fmt.Sprintf("archive '%v'", dump.OutputOptions.Archive)
		}
	}

	if !dump.OutputOptions.Repair {
		log.Logvf(log.Always, "writing %v to %v", intent.Namespace(), intent.Location)
		if dumpCount, err = dump.dumpQueryToIntent(findQuery, intent, buffer); err != nil {
			return err
		}
	} else {
		// handle repairs as a special case, since we cannot count them
		log.Logvf(log.Always, "writing repair of %v to %v", intent.Namespace(), intent.Location)
		repairIter := session.DB(intent.DB).C(intent.C).Repair()
		repairCounter := progress.NewCounter(1) // this counter is ignored
		if err := dump.dumpIterToWriter(repairIter, buffer, repairCounter); err != nil {
			return fmt.Errorf("repair error: %v", err)
		}
		_, repairCount := repairCounter.Progress()
		log.Logvf(log.Always, "\trepair cursor found %v %v in %v",
			repairCount, docPlural(repairCount), intent.Namespace())
	}

	log.Logvf(log.Always, "done dumping %v (%v %v)", intent.Namespace(), dumpCount, docPlural(dumpCount))
	return nil
}

// dumpQueryToIntent takes an mgo Query, its intent, and a writer, performs the query,
// and writes the raw bson results to the writer. Returns a final count of documents
// dumped, and any errors that occured.
func (dump *MongoDump) dumpQueryToIntent(
	query *mgo.Query, intent *intents.Intent, buffer resettableOutputBuffer) (dumpCount int64, err error) {

	// restore of views from archives require an empty collection as the trigger to create the view
	// so, we open here before the early return if IsView so that we write an empty collection to the archive
	err = intent.BSONFile.Open()
	if err != nil {
		return 0, err
	}
	defer func() {
		closeErr := intent.BSONFile.Close()
		if err == nil && closeErr != nil {
			err = fmt.Errorf("error writing data for collection `%v` to disk: %v", intent.Namespace(), closeErr)
		}
	}()
	// don't dump any data for views being dumped as views
	if intent.IsView() && !dump.OutputOptions.ViewsAsCollections {
		return 0, nil
	}
	var total int
	if len(dump.query) == 0 {
		total, err = query.Count()
		if err != nil {
			return int64(0), fmt.Errorf("error reading from db: %v", err)
		}
		log.Logvf(log.DebugLow, "counted %v %v in %v", total, docPlural(int64(total)), intent.Namespace())
	} else {
		log.Logvf(log.DebugLow, "not counting query on %v", intent.Namespace())
	}

	dumpProgressor := progress.NewCounter(int64(total))
	if dump.ProgressManager != nil {
		dump.ProgressManager.Attach(intent.Namespace(), dumpProgressor)
		defer dump.ProgressManager.Detach(intent.Namespace())
	}

	var f io.Writer
	f = intent.BSONFile
	if buffer != nil {
		buffer.Reset(f)
		f = buffer
		defer func() {
			closeErr := buffer.Close()
			if err == nil && closeErr != nil {
				err = fmt.Errorf("error writing data for collection `%v` to disk: %v", intent.Namespace(), closeErr)
			}
		}()
	}

	err = dump.dumpIterToWriter(query.Iter(), f, dumpProgressor)
	dumpCount, _ = dumpProgressor.Progress()
	if err != nil {
		err = fmt.Errorf("error writing data for collection `%v` to disk: %v", intent.Namespace(), err)
	}
	return
}

// dumpIterToWriter takes an mgo iterator, a writer, and a pointer to
// a counter, and dumps the iterator's contents to the writer.
func (dump *MongoDump) dumpIterToWriter(
	iter *mgo.Iter, writer io.Writer, progressCount progress.Updateable) error {
	var termErr error

	// We run the result iteration in its own goroutine,
	// this allows disk i/o to not block reads from the db,
	// which gives a slight speedup on benchmarks
	buffChan := make(chan []byte)
	go func() {
		for {
			select {
			case <-dump.shutdownIntentsNotifier.notified:
				log.Logvf(log.DebugHigh, "terminating writes")
				termErr = util.ErrTerminated
				close(buffChan)
				return
			default:
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
		}
	}()

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
		_, err := writer.Write(buff)
		if err != nil {
			return fmt.Errorf("error writing to file: %v", err)
		}
		progressCount.Inc(1)
	}
	return termErr
}

// DumpUsersAndRolesForDB queries and dumps the users and roles tied to the given
// database. Only works with an authentication schema version >= 3.
func (dump *MongoDump) DumpUsersAndRolesForDB(db string) error {
	session, err := dump.SessionProvider.GetSession()
	buffer := dump.getResettableOutputBuffer()
	if err != nil {
		return err
	}
	defer session.Close()

	dbQuery := bson.M{"db": db}
	usersQuery := session.DB("admin").C("system.users").Find(dbQuery)
	_, err = dump.dumpQueryToIntent(usersQuery, dump.manager.Users(), buffer)
	if err != nil {
		return fmt.Errorf("error dumping db users: %v", err)
	}

	rolesQuery := session.DB("admin").C("system.roles").Find(dbQuery)
	_, err = dump.dumpQueryToIntent(rolesQuery, dump.manager.Roles(), buffer)
	if err != nil {
		return fmt.Errorf("error dumping db roles: %v", err)
	}

	versionQuery := session.DB("admin").C("system.version").Find(nil)
	_, err = dump.dumpQueryToIntent(versionQuery, dump.manager.AuthVersion(), buffer)
	if err != nil {
		return fmt.Errorf("error dumping db auth version: %v", err)
	}

	return nil
}

// DumpUsersAndRoles dumps all of the users and roles and versions
// TODO: This and DumpUsersAndRolesForDB should be merged, correctly
func (dump *MongoDump) DumpUsersAndRoles() error {
	var err error
	buffer := dump.getResettableOutputBuffer()
	if dump.manager.Users() != nil {
		err = dump.DumpIntent(dump.manager.Users(), buffer)
		if err != nil {
			return err
		}
	}
	if dump.manager.Roles() != nil {
		err = dump.DumpIntent(dump.manager.Roles(), buffer)
		if err != nil {
			return err
		}
	}
	if dump.manager.AuthVersion() != nil {
		err = dump.DumpIntent(dump.manager.AuthVersion(), buffer)
		if err != nil {
			return err
		}
	}

	return nil
}

// DumpSystemIndexes dumps all of the system.indexes
func (dump *MongoDump) DumpSystemIndexes() error {
	buffer := dump.getResettableOutputBuffer()
	for _, dbName := range dump.manager.SystemIndexDBs() {
		err := dump.DumpIntent(dump.manager.SystemIndexes(dbName), buffer)
		if err != nil {
			return err
		}
	}
	return nil
}

// DumpMetadata dumps the metadata for each intent in the manager
// that has metadata
func (dump *MongoDump) DumpMetadata() error {
	allIntents := dump.manager.Intents()
	buffer := dump.getResettableOutputBuffer()
	for _, intent := range allIntents {
		if intent.MetadataFile != nil {
			err := dump.dumpMetadata(intent, buffer)
			if err != nil {
				return err
			}
		}
	}
	return nil
}

// nopCloseWriter implements io.WriteCloser. It wraps up a io.Writer, and adds a no-op Close
type nopCloseWriter struct {
	io.Writer
}

// Close does nothing on nopCloseWriters
func (*nopCloseWriter) Close() error {
	return nil
}

func (dump *MongoDump) getArchiveOut() (out io.WriteCloser, err error) {
	if dump.OutputOptions.Archive == "-" {
		out = &nopCloseWriter{dump.OutputWriter}
	} else {
		targetStat, err := os.Stat(dump.OutputOptions.Archive)
		if err == nil && targetStat.IsDir() {
			defaultArchiveFilePath :=
				filepath.Join(dump.OutputOptions.Archive, "archive")
			if dump.OutputOptions.Gzip {
				defaultArchiveFilePath = defaultArchiveFilePath + ".gz"
			}
			out, err = os.Create(defaultArchiveFilePath)
			if err != nil {
				return nil, err
			}
		} else {
			out, err = os.Create(dump.OutputOptions.Archive)
			if err != nil {
				return nil, err
			}
		}
	}
	if dump.OutputOptions.Gzip {
		return &util.WrappedWriteCloser{gzip.NewWriter(out), out}, nil
	}
	return out, nil
}

// docPlural returns "document" or "documents" depending on the
// count of documents passed in.
func docPlural(count int64) string {
	return util.Pluralize(int(count), "document", "documents")
}

func (dump *MongoDump) HandleInterrupt() {
	if dump.shutdownIntentsNotifier != nil {
		dump.shutdownIntentsNotifier.Notify()
	}
}

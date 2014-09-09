package mongodump

import (
	"bufio"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/auth"
	"github.com/shelman/mongo-tools-proto/common/db"
	"github.com/shelman/mongo-tools-proto/common/json"
	"github.com/shelman/mongo-tools-proto/common/log"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/progress"
	"github.com/shelman/mongo-tools-proto/mongodump/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const ProgressBarLength = 24

type MongoDump struct {
	// basic mongo tool options
	ToolOptions *commonopts.ToolOptions

	InputOptions  *options.InputOptions
	OutputOptions *options.OutputOptions

	SessionProvider *db.SessionProvider

	// useful internals that we don't directly expose as options
	useStdout       bool
	query           bson.M
	oplogCollection string
	authVersion     int
}

// ValidateOptions checks for any incompatible sets of options
func (dump *MongoDump) ValidateOptions() error {
	switch {
	case dump.OutputOptions.Out == "-" && dump.ToolOptions.Collection == "":
		return fmt.Errorf("can only dump a single collection to stdout")
	case dump.ToolOptions.DB == "" && dump.ToolOptions.Collection != "":
		return fmt.Errorf("cannot dump a collection without a specified database")
	case dump.InputOptions.Query != "" && dump.ToolOptions.Collection == "":
		return fmt.Errorf("cannot dump using a query without a specified collection")
	case dump.OutputOptions.DumpDBUsersAndRoles && dump.ToolOptions.DB == "":
		return fmt.Errorf("must specify a database when running with dumpDbUsersAndRoles")
	case dump.OutputOptions.DumpDBUsersAndRoles && dump.ToolOptions.Collection != "":
		return fmt.Errorf("cannot specify a collection when running with dumpDbUsersAndRoles") //TODO: why?
	case dump.OutputOptions.Oplog && dump.ToolOptions.DB != "":
		return fmt.Errorf("--oplog mode only supported on full dumps")
	}
	return nil
}

// Dump handles some final options checking and executes MongoDump
func (dump *MongoDump) Dump() error {
	err := dump.ValidateOptions()
	if err != nil {
		return fmt.Errorf("Bad Option: %v", err)
	}

	if dump.InputOptions.Query != "" {
		// TODO, check for extended json support...
		// gonna need to do some exploring later on, since im 95% sure
		// this is undefined in the current tools
		err = json.Unmarshal([]byte(dump.InputOptions.Query), &dump.query)
		if err != nil {
			return fmt.Errorf("error parsing query: %v", err)
		}
	}

	if dump.OutputOptions.Out == "-" {
		dump.useStdout = true
	}

	if dump.OutputOptions.DumpDBUsersAndRoles {
		//first make sure this is possible with the connected database
		dump.authVersion, err = auth.GetAuthVersion(dump.SessionProvider.GetSession())
		if err != nil {
			return fmt.Errorf("error getting auth schema version for dumpDbUsersAndRoles: %v", err)
		}
		log.Logf(2, "using auth schema version %v", dump.authVersion)
		if dump.authVersion != 3 {
			return fmt.Errorf("backing up users and roles is only supported for "+
				"deployments with auth schema versions 3, found: %v", dump.authVersion)
		}
	}

	//switch on what kind of execution to do
	switch {
	case dump.ToolOptions.DB == "" && dump.ToolOptions.Collection == "":
		err = dump.DumpEverything()
	case dump.ToolOptions.DB != "" && dump.ToolOptions.Collection == "":
		err = dump.DumpDatabase(dump.ToolOptions.DB)
	case dump.ToolOptions.DB != "" && dump.ToolOptions.Collection != "":
		err = dump.DumpCollection(dump.ToolOptions.DB, dump.ToolOptions.Collection)
	}

	if dump.OutputOptions.DumpDBUsersAndRoles {
		log.Logf(0, "dumping users and roles for %v", dump.ToolOptions.DB)
		if dump.ToolOptions.DB == "admin" {
			log.Logf(0, "skipping users/roles dump, already dumped admin database")
		} else {
			err = dump.DumpUsersAndRolesForDB(dump.ToolOptions.DB)
			if err != nil {
				return fmt.Errorf("error dumping users and roles: %v", err)
			}
		}
	}

	log.Logf(1, "done")

	return err
}

// DumpEverything dumps all found databases and handles the oplog,
// skipping the "local" db, which can only be explicitly dumped.
func (dump *MongoDump) DumpEverything() error {
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
		log.Logf(1, "getting most recent oplog timestamp")
		oplogStart, err = dump.getOplogStartTime()
		if err != nil {
			return fmt.Errorf("error getting oplog start: %v", err)
		}
	}

	// Iterate over all database names, dumping them
	session := dump.SessionProvider.GetSession()
	dbs, err := session.DatabaseNames()
	if err != nil {
		return fmt.Errorf("error getting database names: %v", err)
	}
	for _, dbName := range dbs {
		if dbName != "local" { // local can only be explicitly dumped
			log.Logf(0, "dumping database %v", dbName)
			err := dump.DumpDatabase(dbName)
			if err != nil {
				return err
			}
		}
	}

	// If we are capturing the oplog, we dump all oplog entries that occurred
	// while dumping the database. Before and after dumping the oplog,
	// we check to see if the oplog has rolled over (i.e. the most recent entry when
	// we started still exist, so we know we haven't lost data)
	if dump.OutputOptions.Oplog {
		log.Logf(2, "checking if oplog entry %v still exists", oplogStart)
		exists, err := dump.checkOplogTimestampExists(oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logf(3, "oplog entry %v still exists", oplogStart)

		// dump oplog in root of the dump folder
		oplogFilepath := filepath.Join(dump.OutputOptions.Out, "oplog.bson")
		oplogOut, err := os.Create(oplogFilepath)
		if err != nil {
			return fmt.Errorf("error creating bson file `%v`: %v", oplogFilepath, err)
		}

		log.Logf(0, "writing captured oplog to %v", oplogFilepath)
		session.SetPrefetch(1.0) //mimic exhaust cursor
		oplogQuery := session.DB("local").C(dump.oplogCollection).Find(
			bson.M{
				"ts": bson.M{
					"$gt": oplogStart,
				},
			}).LogReplay()
		err = dump.dumpQueryToWriter(oplogQuery, oplogOut)
		if err != nil {
			return err
		}

		// check the oplog for a rollover one last time, to avoid a race condition
		// wherein the oplog rolls over in the time after our first check, but before
		// we copy it.
		log.Logf(2, "checking again if oplog entry %v still exists", oplogStart)
		exists, err = dump.checkOplogTimestampExists(oplogStart)
		if !exists {
			return fmt.Errorf(
				"oplog overflow: mongodump was unable to capture all new oplog entries during execution")
		}
		if err != nil {
			return fmt.Errorf("unable to check oplog for overflow: %v", err)
		}
		log.Logf(3, "oplog entry %v still exists", oplogStart)
	}

	return nil
}

// DumpDatabase dumps the specified database
func (dump *MongoDump) DumpDatabase(db string) error {
	session := dump.SessionProvider.GetSession()

	cols, err := session.DB(db).CollectionNames()
	if err != nil {
		return fmt.Errorf("error getting collections names for database `%v`: %v", dump.ToolOptions.DB, err)
	}
	log.Logf(2, "found collections: %v", strings.Join(cols, ", "))
	for _, col := range cols {
		err = dump.DumpCollection(db, col)
		if err != nil {
			return err
		}
	}
	return nil
}

// DumpCollection dumps the specified database's collection
func (dump *MongoDump) DumpCollection(db, c string) error {
	session := dump.SessionProvider.GetSession()
	// in mgo, setting prefetch = 1.0 causes the driver to make requests for
	// more results as soon as results are returned. This effectively
	// duplicates the behavior of an exhaust cursor.
	session.SetPrefetch(1.0)

	var findQuery *mgo.Query
	switch {
	case len(dump.query) > 0:
		findQuery = session.DB(db).C(c).Find(dump.query)
	case dump.InputOptions.TableScan:
		// ---forceTablesScan runs the query without snapshot enabled
		findQuery = session.DB(db).C(c).Find(nil)
	default:
		findQuery = session.DB(db).C(c).Find(nil).Snapshot()
	}

	if dump.useStdout {
		log.Logf(0, "writing %v.%v to stdout", db, c)
		return dump.dumpQueryToWriter(findQuery, os.Stdout)
	} else {
		dbFolder := filepath.Join(dump.OutputOptions.Out, db)
		err := os.MkdirAll(dbFolder, 0755)
		if err != nil {
			return fmt.Errorf("error creating directory `%v`: %v", dbFolder, err)
		}

		outFilepath := filepath.Join(dbFolder, fmt.Sprintf("%v.bson", c))
		out, err := os.Create(outFilepath)
		if err != nil {
			return fmt.Errorf("error creating bson file `%v`: %v", outFilepath, err)
		}
		defer out.Close()

		log.Logf(0, "writing %v.%v to %v", db, c, outFilepath)
		err = dump.dumpQueryToWriter(findQuery, out)
		if err != nil {
			return err
		}

		metadataFilepath := filepath.Join(dbFolder, fmt.Sprintf("%v.metadata.json", c))
		metaOut, err := os.Create(metadataFilepath)
		if err != nil {
			return fmt.Errorf("error creating metadata.json file `%v`: %v", outFilepath, err)
		}
		defer metaOut.Close()

		log.Logf(0, "writing %v.%v metadata to %v", db, c, metadataFilepath)
		return dump.dumpMetadataToWriter(db, c, metaOut)
	}
}

// dumpQueryToWriter takes an mgo Query and a writer, performs the query,
// and writes the raw bson results to the writer.
func (dump *MongoDump) dumpQueryToWriter(query *mgo.Query, writer io.Writer) error {
	var dumpCounter int
	total, err := query.Count()
	if err != nil {
		return fmt.Errorf("error reading from db: %v", err)
	}
	log.Logf(1, "\t%v documents", total)

	bar := progress.ProgressBar{
		Max:        total,
		CounterPtr: &dumpCounter,
		WaitTime:   3 * time.Second,
		Writer:     log.Writer(0),
		BarLength:  ProgressBarLength,
	}
	bar.Start()
	defer bar.Stop()

	cursor := query.Iter()
	defer cursor.Close()

	// We run the result iteration in its own goroutine,
	// this allows disk i/o to not block reads from the db,
	// which gives a slight speedup on benchmarks
	buffChan := make(chan []byte)
	go func() {
		for {
			raw := &bson.Raw{}
			if err := cursor.Err(); err != nil {
				log.Logf(0, "error reading from db: %v", err)
			}
			next := cursor.Next(raw)
			if !next {
				close(buffChan)
				return
			}
			buffChan <- raw.Data
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
			if cursor.Err() != nil {
				return fmt.Errorf("error reading collection: %v", cursor.Err())
			}
			break
		}
		_, err := w.Write(buff)
		if err != nil {
			return fmt.Errorf("error writing to file: %v", err)
		}
		dumpCounter++
	}
	err = w.Flush()
	if err != nil {
		return fmt.Errorf("error flushing file writer: %v", err)
	}
	return nil
}

// DumpUsersAndRolesForDB queries and dumps the users and roles tied
// to the given the db. Only works with schema version == 3
func (dump *MongoDump) DumpUsersAndRolesForDB(db string) error {
	session := dump.SessionProvider.GetSession()
	dbQuery := bson.M{"db": db}
	outDir := filepath.Join(dump.OutputOptions.Out, db)

	usersFile, err := os.Create(filepath.Join(outDir, "$admin.system.users"))
	if err != nil {
		return fmt.Errorf("error creating file for db users: %v", err)
	}
	usersQuery := session.DB("admin").C("system.users").Find(dbQuery)
	err = dump.dumpQueryToWriter(usersQuery, usersFile)
	if err != nil {
		return fmt.Errorf("error dumping db users: %v", err)
	}

	rolesFile, err := os.Create(filepath.Join(outDir, "$admin.system.roles"))
	if err != nil {
		return fmt.Errorf("error creating file for db roles: %v", err)
	}
	rolesQuery := session.DB("admin").C("system.roles").Find(dbQuery)
	err = dump.dumpQueryToWriter(rolesQuery, rolesFile)
	if err != nil {
		return fmt.Errorf("error dumping db roles: %v", err)
	}

	return nil
}

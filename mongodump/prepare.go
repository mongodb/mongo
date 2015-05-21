package mongodump

import (
	"bufio"
	"bytes"
	"fmt"
	"github.com/mongodb/mongo-tools/common/archive"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type collectionInfo struct {
	Name    string  `bson:"name"`
	Options *bson.D `bson:"options"`
}

// realBSONFile implements the intents.file interface. It lets intents write to real BSON files
// ok disk via an embedded bufio.Writer
// The Write method of the intents.file interface is implemented here by the embedded bufio.Writer
type realBSONFile struct {
	*bufio.Writer
	file   *os.File
	intent *intents.Intent
}

// Open is part of the intents.file interface. realBSONFiles need to have Open called before
// Read can be called
func (f *realBSONFile) Open() (err error) {
	if f.intent.BSONPath == "" {
		// This should not occur normally. All intents should have a BSONPath.
		return fmt.Errorf("error creating BSON file without a path, namespace: %v",
			f.intent.Namespace())
	}
	err = os.MkdirAll(filepath.Dir(f.intent.BSONPath), os.ModeDir|os.ModePerm)
	if err != nil {
		return fmt.Errorf("error creating directory for BSON file %v: %v",
			filepath.Dir(f.intent.BSONPath), err)
	}
	f.file, err = os.Create(f.intent.BSONPath)
	if err != nil {
		return fmt.Errorf("error creating BSON file %v: %v", f.intent.BSONPath, err)
	}

	// wrap writer in buffer to reduce load on disk
	f.Writer = bufio.NewWriterSize(f.file, 32*1024)

	return nil
}

// Read is is part of the intents.file interface, Read on realBSONFile shouldn't be used.r
// We could probabbly justifiably panic here.
func (f *realBSONFile) Read([]byte) (int, error) {
	return 0, io.EOF
}

// Close is part of the intents.file interface, Close on realBSONFiles gets called in DumpIntent
func (f *realBSONFile) Close() error {
	err := f.Writer.Flush()
	if err != nil {
		return err
	}
	return f.file.Close()
}

type realMetadataFile struct {
	*os.File
	intent *intents.Intent
}

func (f *realMetadataFile) Open() (err error) {
	if f.intent.MetadataPath == "" {
		return fmt.Errorf("No MetadataPath for %v.%v", f.intent.DB, f.intent.C)
	}
	err = os.MkdirAll(filepath.Dir(f.intent.MetadataPath), os.ModeDir|os.ModePerm)
	if err != nil {
		return fmt.Errorf("error creating directory for Metadata file %v: %v",
			filepath.Dir(f.intent.MetadataPath), err)
	}
	f.File, err = os.Create(f.intent.MetadataPath)
	if err != nil {
		return fmt.Errorf("error creating Metadata file %v: %v",
			f.intent.MetadataPath, err)
	}
	return nil
}

// stdoutFile implements the intents.file interface. stdoutFiles are used when single collections
// are written directly (non-archive-mode) to standard out, via "--dir -"
type stdoutFile struct {
	*os.File
	intent *intents.Intent
}

// Open is part of the intents.file interface.
func (f *stdoutFile) Open() error {
	f.File = os.Stdout
	return nil
}

// Close is part of the intents.file interface. While we could actually close os.Stdout here,
// that's actually a bad idea. Unsetting f.File here will cause future writes to fail, but that
// shouldn't happen anyway.
func (f *stdoutFile) Close() error {
	f.File = nil
	return nil
}

// Read is part of the intents.file interface. Nobody should be reading from stdoutFiles,
// could probably justifiably panic here.
func (f *stdoutFile) Read(p []byte) (n int, err error) {
	return 0, fmt.Errorf("can't read from standard output")
}

// shouldSkipCollection returns true when a collection name is excluded
// by the mongodump options.
func (dump *MongoDump) shouldSkipCollection(colName string) bool {
	for _, excludedCollection := range dump.OutputOptions.ExcludedCollections {
		if colName == excludedCollection {
			return true
		}
	}
	for _, excludedCollectionPrefix := range dump.OutputOptions.ExcludedCollectionPrefixes {
		if strings.HasPrefix(colName, excludedCollectionPrefix) {
			return true
		}
	}
	return false
}

// outputPath creates a path for the collection to be written to (sans file extension).
func (dump *MongoDump) outputPath(dbName, colName string) string {
	var root string
	if dump.OutputOptions.Out == "" {
		root = "dump"
	} else {
		root = dump.OutputOptions.Out
	}
	if dbName == "" {
		return filepath.Join(root, colName)
	}
	return filepath.Join(root, dbName, colName)
}

func checkStringForPathSeparator(s string, c *rune) bool {
	for _, *c = range s {
		if os.IsPathSeparator(uint8(*c)) {
			return true
		}
	}
	return false
}

// NewIntent creates a bare intent without populating the options.
func (dump *MongoDump) NewIntent(dbName, colName string, stdout bool) (*intents.Intent, error) {
	intent := &intents.Intent{
		DB:       dbName,
		C:        colName,
		BSONPath: dump.outputPath(dbName, colName) + ".bson",
	}

	// add stdout flags if we're using stdout
	if dump.useStdout {
		intent.BSONFile = &stdoutFile{intent: intent}
		intent.MetadataFile = &stdoutFile{intent: intent}
	}

	if dump.OutputOptions.Archive != "" {
		intent.BSONFile = &archive.MuxIn{Intent: intent, Mux: dump.archive.Mux}
	} else {
		var c rune
		if checkStringForPathSeparator(colName, &c) || checkStringForPathSeparator(dbName, &c) {
			return nil, fmt.Errorf(`"%v.%v" contains a path separator '%c' `+
				`and can't be dumped to the filesystem`, dbName, colName, c)
		}
		intent.BSONFile = &realBSONFile{intent: intent}
	}

	if !intent.IsSystemIndexes() {
		intent.MetadataPath = dump.outputPath(dbName, colName+".metadata.json")
		if dump.OutputOptions.Archive != "" {
			intent.MetadataFile = &archive.MetadataFile{
				Intent: intent,
				Buffer: &bytes.Buffer{},
			}
		} else {
			intent.MetadataFile = &realMetadataFile{intent: intent}
		}
	}

	// get a document count for scheduling purposes
	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()

	count, err := session.DB(dbName).C(colName).Count()
	if err != nil {
		return nil, fmt.Errorf("error counting %v: %v", intent.Namespace(), err)
	}
	intent.Size = int64(count)

	return intent, nil
}

// CreateOplogIntents creates an intents.Intent for the oplog and adds it to the manager
func (dump *MongoDump) CreateOplogIntents() error {

	err := dump.determineOplogCollectionName()
	if err != nil {
		return err
	}

	oplogIntent := &intents.Intent{
		DB:       "local",
		C:        dump.oplogCollection,
		BSONPath: dump.outputPath("oplog.bson", ""),
	}
	if dump.OutputOptions.Archive != "" {
		oplogIntent.BSONFile = &archive.MuxIn{Mux: dump.archive.Mux, Intent: oplogIntent}
	} else {
		oplogIntent.BSONFile = &realBSONFile{intent: oplogIntent}
	}
	dump.manager.Put(oplogIntent)
	return nil
}

// CreateUsersRolesVersionIntentsForDB create intents to be written in to the specific
// database folder, for the users, roles and version admin database collections
// And then it adds the intents in to the manager
func (dump *MongoDump) CreateUsersRolesVersionIntentsForDB(db string) error {

	outDir := dump.outputPath(db, "")

	usersIntent := &intents.Intent{
		DB:       db,
		C:        "$admin.system.users",
		BSONPath: filepath.Join(outDir, "$admin.system.users.bson"),
	}
	rolesIntent := &intents.Intent{
		DB:       db,
		C:        "$admin.system.roles",
		BSONPath: filepath.Join(outDir, "$admin.system.roles.bson"),
	}
	versionIntent := &intents.Intent{
		DB:       db,
		C:        "$admin.system.version",
		BSONPath: filepath.Join(outDir, "$admin.system.version.bson"),
	}
	if dump.OutputOptions.Archive != "" {
		usersIntent.BSONFile = &archive.MuxIn{Intent: usersIntent, Mux: dump.archive.Mux}
		rolesIntent.BSONFile = &archive.MuxIn{Intent: rolesIntent, Mux: dump.archive.Mux}
		versionIntent.BSONFile = &archive.MuxIn{Intent: versionIntent, Mux: dump.archive.Mux}
	} else {
		usersIntent.BSONFile = &realBSONFile{intent: usersIntent}
		rolesIntent.BSONFile = &realBSONFile{intent: rolesIntent}
		versionIntent.BSONFile = &realBSONFile{intent: versionIntent}
	}
	dump.manager.Put(usersIntent)
	dump.manager.Put(rolesIntent)
	dump.manager.Put(versionIntent)

	return nil
}

// CreateCollectionIntent builds an intent for a given collection and
// puts it into the intent manager.
func (dump *MongoDump) CreateCollectionIntent(dbName, colName string) error {
	if dump.shouldSkipCollection(colName) {
		log.Logf(log.DebugLow, "skipping dump of %v.%v, it is excluded", dbName, colName)
		return nil
	}

	intent, err := dump.NewIntent(dbName, colName, dump.useStdout)
	if err != nil {
		return err
	}

	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	opts, err := db.GetCollectionOptions(session.DB(dbName).C(colName))
	if err != nil {
		return fmt.Errorf("error getting collection options: %v", err)
	}

	intent.Options = nil
	if opts != nil {
		optsInterface, _ := bsonutil.FindValueByKey("options", opts)
		if optsInterface != nil {
			if optsD, ok := optsInterface.(bson.D); ok {
				intent.Options = &optsD
			} else {
				return fmt.Errorf("Failed to parse collection options as bson.D")
			}
		}
	}

	dump.manager.Put(intent)

	log.Logf(log.DebugLow, "enqueued collection '%v'", intent.Namespace())
	return nil
}

func (dump *MongoDump) createIntentFromOptions(dbName string, ci *collectionInfo) error {
	if dump.shouldSkipCollection(ci.Name) {
		log.Logf(log.DebugLow, "skipping dump of %v.%v, it is excluded", dbName, ci.Name)
		return nil
	}
	intent, err := dump.NewIntent(dbName, ci.Name, dump.useStdout)
	if err != nil {
		return err
	}
	intent.Options = ci.Options
	dump.manager.Put(intent)
	log.Logf(log.DebugLow, "enqueued collection '%v'", intent.Namespace())
	return nil
}

// CreateIntentsForDatabase iterates through collections in a db
// and builds dump intents for each collection.
func (dump *MongoDump) CreateIntentsForDatabase(dbName string) error {
	// we must ensure folders for empty databases are still created, for legacy purposes

	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	colsIter, fullName, err := db.GetCollections(session.DB(dbName), "")
	if err != nil {
		return fmt.Errorf("error getting collections for database `%v`: %v", dbName, err)
	}

	collInfo := &collectionInfo{}
	for colsIter.Next(collInfo) {
		// Skip over indexes since they are also listed in system.namespaces in 2.6 or earlier
		if strings.Contains(collInfo.Name, "$") && !strings.Contains(collInfo.Name, ".oplog.$") {
			continue
		}
		if fullName {
			namespacePrefix := dbName + "."
			// if the collection info came from querying system.indexes (2.6 or earlier) then the
			// "name" we get includes the db name as well, so we must remove it
			if strings.HasPrefix(collInfo.Name, namespacePrefix) {
				collInfo.Name = collInfo.Name[len(namespacePrefix):]
			} else {
				return fmt.Errorf("namespace '%v' format is invalid - expected to start with '%v'", collInfo.Name, namespacePrefix)
			}
		}
		err := dump.createIntentFromOptions(dbName, collInfo)
		if err != nil {
			return err
		}
	}
	return colsIter.Err()
}

// CreateAllIntents iterates through all dbs and collections and builds
// dump intents for each collection.
func (dump *MongoDump) CreateAllIntents() error {
	dbs, err := dump.sessionProvider.DatabaseNames()
	if err != nil {
		return fmt.Errorf("error getting database names: %v", err)
	}
	log.Logf(log.DebugHigh, "found databases: %v", strings.Join(dbs, ", "))
	for _, dbName := range dbs {
		if dbName == "local" {
			// local can only be explicitly dumped
			continue
		}
		if err := dump.CreateIntentsForDatabase(dbName); err != nil {
			return err
		}
	}
	return nil
}

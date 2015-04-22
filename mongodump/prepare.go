package mongodump

import (
	"bytes"
	"fmt"
	"github.com/mongodb/mongo-tools/common/archive"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2/bson"
	"os"
	"path/filepath"
	"strings"
)

type collectionInfo struct {
	Name    string  `bson:"name"`
	Options *bson.D `bson:"options"`
}

type bsonFileFile struct {
	*os.File
	intent *intents.Intent
}

func (f *bsonFileFile) Open() (err error) {
	if f.intent.BSONPath == "" {
		return fmt.Errorf("No BSONPath for %v.%v", f.intent.DB, f.intent.C)
	}
	f.File, err = os.Create(f.intent.BSONPath)
	if err != nil {
		return fmt.Errorf("error creating BSON file %v: %v", f.intent.BSONPath, err)
	}
	return nil
}

type metadataFileFile struct {
	*os.File
	intent *intents.Intent
}

func (f *metadataFileFile) Open() (err error) {
	if f.intent.MetadataPath == "" {
		return fmt.Errorf("No MetadataPath for %v.%v", f.intent.DB, f.intent.C)
	}
	f.File, err = os.Create(f.intent.MetadataPath)
	if err != nil {
		return fmt.Errorf("error creating Metadata file %v: %v", f.intent.MetadataPath, err)
	}
	return nil
}

type stdoutFile struct {
	*os.File
	intent *intents.Intent
}

func (f *stdoutFile) Open() error {
	f.File = os.Stdout
	return nil
}

func (f *stdoutFile) Close() error {
	f.File = nil
	return nil
}

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
	return filepath.Join(dump.OutputOptions.Out, dbName, colName)
}

// NewIntent creates a bare intent without populating the options.
func (dump *MongoDump) NewIntent(dbName, collName string, stdout bool) (*intents.Intent, error) {
	intent := &intents.Intent{
		DB:       dbName,
		C:        collName,
		BSONPath: dump.outputPath(dbName, collName) + ".bson",
	}

	// add stdout flags if we're using stdout
	if dump.useStdout {
		intent.BSONFile = &stdoutFile{intent: intent}
		// We don't actually need a stdoutMetadataFile type because none of the methods on the stdoutFile
		// Make any use of the BSON or Metadata parts of the intent
		intent.MetadataFile = &stdoutFile{intent: intent}
	}

	if dump.OutputOptions.Archive {
		intent.BSONFile = &archive.MuxIn{Intent: intent, Mux: dump.archive.Mux}
	} else {
		intent.BSONFile = &bsonFileFile{intent: intent}
	}

	if !intent.IsSystemIndexes() {
		intent.MetadataPath = dump.outputPath(dbName, colName+".metadata.json")
		if dump.OutputOptions.Archive {
			intent.MetadataFile = &archive.Metadata{
				Intent: intent,
				Buffer: &bytes.Buffer{},
			}
		} else {
			intent.MetadataFile = &metadataFileFile{intent: intent}
		}
	}

	// get a document count for scheduling purposes
	session, err := dump.sessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()

	count, err := session.DB(dbName).C(collName).Count()
	if err != nil {
		return nil, fmt.Errorf("error counting %v: %v", intent.Namespace(), err)
	}
	intent.Size = int64(count)

	return intent, nil
}

func (dump *MongoDump) CreateOplogIntents() error {

	err := dump.determineOplogCollectionName()
	if err != nil {
		return err
	}

	err = os.MkdirAll(dump.OutputOptions.Out, defaultPermissions)
	if err != nil {
		return err
	}

	oplogIntent := &intents.Intent{
		DB:       "local",
		C:        dump.oplogCollection,
		BSONPath: filepath.Join(dump.OutputOptions.Out, "oplog.bson"),
	}
	if dump.OutputOptions.Archive {
		oplogIntent.BSONFile = &archive.MuxIn{Mux: dump.archive.Mux, Intent: oplogIntent}
	} else {
		oplogIntent.BSONFile = &bsonFileFile{intent: oplogIntent}
	}
	dump.manager.Put(oplogIntent)
	return nil
}

// CreateUsersRolesVersionIntentsForDB create intents to be written in to the specific
// collection folder, for the users, roles and version admin database collections
// And then it adds the intents in to the manager
func (dump *MongoDump) CreateUsersRolesVersionIntentsForDB(db string) error {

	outDir := filepath.Join(dump.OutputOptions.Out, db)
	err := os.MkdirAll(outDir, defaultPermissions)
	if err != nil {
		return err
	}

	usersIntent := &intents.Intent{
		DB:       "admin",
		C:        "system.users",
		BSONPath: filepath.Join(outDir, "$admin.system.users.bson"),
	}
	rolesIntent := &intents.Intent{
		DB:       "admin",
		C:        "system.roles",
		BSONPath: filepath.Join(outDir, "$admin.system.roles.bson"),
	}
	versionIntent := &intents.Intent{
		DB:       "admin",
		C:        "system.version",
		BSONPath: filepath.Join(outDir, "$admin.system.version.bson"),
	}
	if dump.OutputOptions.Archive {
		usersIntent.BSONFile = &archive.MuxIn{Intent: usersIntent, Mux: dump.archive.Mux}
		rolesIntent.BSONFile = &archive.MuxIn{Intent: rolesIntent, Mux: dump.archive.Mux}
		versionIntent.BSONFile = &archive.MuxIn{Intent: versionIntent, Mux: dump.archive.Mux}
	} else {
		usersIntent.BSONFile = &bsonFileFile{intent: usersIntent}
		rolesIntent.BSONFile = &bsonFileFile{intent: rolesIntent}
		versionIntent.BSONFile = &bsonFileFile{intent: versionIntent}
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
	dbFolder := filepath.Join(dump.OutputOptions.Out, dbName)
	// XXX move to the open
	err := os.MkdirAll(dbFolder, defaultPermissions)
	if err != nil {
		return fmt.Errorf("error creating directory `%v`: %v", dbFolder, err)
	}

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

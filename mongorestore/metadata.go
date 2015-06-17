package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"os"
	"strings"
)

// Specially treated restore collection types.
const (
	Users = "users"
	Roles = "roles"
)

// struct for working with auth versions
type authVersionPair struct {
	// Dump is the auth version of the users/roles collection files in the target dump directory
	Dump int
	// Server is the auth version of the connected MongoDB server
	Server int
}

// Metadata holds information about a collection's options and indexes.
type Metadata struct {
	Options bson.D          `json:"options,omitempty"`
	Indexes []IndexDocument `json:"indexes"`
}

// this struct is used to read in the options of a set of indexes
type metaDataMapIndex struct {
	Indexes []bson.M `json:"indexes"`
}

// IndexDocument holds information about a collection's index.
type IndexDocument struct {
	Options bson.M `bson:",inline"`
	Key     bson.D `bson:"key"`
}

// MetadataFromJSON takes a slice of JSON bytes and unmarshals them into usable
// collection options and indexes for restoring collections.
func (restore *MongoRestore) MetadataFromJSON(jsonBytes []byte) (bson.D, []IndexDocument, error) {
	if len(jsonBytes) == 0 {
		// skip metadata parsing if the file is empty
		return nil, nil, nil
	}

	meta := &Metadata{}

	err := json.Unmarshal(jsonBytes, meta)
	if err != nil {
		return nil, nil, err
	}

	// first get the ordered key information for each index,
	// then merge it with a set of options stored as a map
	metaAsMap := metaDataMapIndex{}
	err = json.Unmarshal(jsonBytes, &metaAsMap)
	if err != nil {
		return nil, nil, fmt.Errorf("error unmarshalling metadata as map: %v", err)
	}
	for i := range meta.Indexes {
		// remove "key" from the map so we can decode it properly later
		delete(metaAsMap.Indexes[i], "key")

		// parse extra index fields
		meta.Indexes[i].Options = metaAsMap.Indexes[i]
		if err := bsonutil.ConvertJSONDocumentToBSON(meta.Indexes[i].Options); err != nil {
			return nil, nil, fmt.Errorf("extended json error: %v", err)
		}

		// parse the values of the index keys, so we can support extended json
		for pos, field := range meta.Indexes[i].Key {
			meta.Indexes[i].Key[pos].Value, err = bsonutil.ParseJSONValue(field.Value)
			if err != nil {
				return nil, nil, fmt.Errorf("extended json in '%v' field: %v", field.Name, err)
			}
		}
	}

	// parse the values of options fields, to support extended json
	meta.Options, err = bsonutil.GetExtendedBsonD(meta.Options)
	if err != nil {
		return nil, nil, fmt.Errorf("extended json in 'options': %v", err)
	}

	return meta.Options, meta.Indexes, nil
}

// IndexesFromBSON extracts index information from BSON files.
func (restore *MongoRestore) IndexesFromBSON(intent *intents.Intent, bsonFile string) ([]IndexDocument, error) {
	log.Logf(log.DebugLow, "scanning %v for indexes on %v collections", bsonFile, intent.C)

	rawFile, err := os.Open(bsonFile)
	if err != nil {
		return nil, fmt.Errorf("error reading index bson file %v: %v", bsonFile, err)
	}

	bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(rawFile))
	defer bsonSource.Close()

	// iterate over stored indexes, saving all that match the collection
	indexDocument := &IndexDocument{}
	collectionIndexes := []IndexDocument{}
	for bsonSource.Next(indexDocument) {
		namespace := indexDocument.Options["ns"].(string)
		if stripDBFromNS(namespace) == intent.C {
			log.Logf(log.DebugHigh, "\tfound index %v", indexDocument.Options["name"])
			collectionIndexes = append(collectionIndexes, *indexDocument)
		}
	}
	if bsonSource.Err() != nil {
		return nil, fmt.Errorf("error scanning system.indexes for %v indexes: %v", intent.C, err)
	}

	return collectionIndexes, nil
}

func stripDBFromNS(ns string) string {
	i := strings.Index(ns, ".")
	if i > 0 && i < len(ns) {
		return ns[i+1:]
	}
	return ns
}

// CollectionExists returns true if the given intent's collection exists.
func (restore *MongoRestore) CollectionExists(intent *intents.Intent) (bool, error) {
	restore.knownCollectionsMutex.Lock()
	defer restore.knownCollectionsMutex.Unlock()

	// make sure the map exists
	if restore.knownCollections == nil {
		restore.knownCollections = map[string][]string{}
	}

	// first check if we haven't done listCollections for this database already
	if restore.knownCollections[intent.DB] == nil {
		// if the database name isn't in the cache, grab collection
		// names from the server
		session, err := restore.SessionProvider.GetSession()
		if err != nil {
			return false, fmt.Errorf("error establishing connection: %v", err)
		}
		defer session.Close()
		collections, err := session.DB(intent.DB).CollectionNames()
		if err != nil {
			return false, err
		}
		// update the cache
		restore.knownCollections[intent.DB] = collections
	}

	// now check the cache for the given collection name
	exists := util.StringSliceContains(restore.knownCollections[intent.DB], intent.C)
	return exists, nil
}

// CreateIndexes takes in an intent and an array of index documents and
// attempts to create them using the createIndexes command. If that command
// fails, we fall back to individual index creation.
func (restore *MongoRestore) CreateIndexes(intent *intents.Intent, indexes []IndexDocument) error {
	// first, sanitize the indexes
	for _, index := range indexes {
		// update the namespace of the index before inserting
		index.Options["ns"] = intent.Namespace()

		// check for length violations before building the command
		fullIndexName := fmt.Sprintf("%v.$%v", index.Options["ns"], index.Options["name"])
		if len(fullIndexName) > 127 {
			return fmt.Errorf(
				"cannot restore index with namespace '%v': "+
					"namespace is too long (max size is 127 bytes)", fullIndexName)
		}

		// remove the index version, forcing an update,
		// unless we specifically want to keep it
		if !restore.OutputOptions.KeepIndexVersion {
			delete(index.Options, "v")
		}
	}

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error establishing connection: %v", err)
	}
	session.SetSafe(&mgo.Safe{})
	defer session.Close()

	// then attempt the createIndexes command
	rawCommand := bson.D{
		{"createIndexes", intent.C},
		{"indexes", indexes},
	}
	results := bson.M{}
	err = session.DB(intent.DB).Run(rawCommand, &results)
	if err == nil {
		return nil
	}
	if err.Error() != "no such cmd: createIndexes" {
		return fmt.Errorf("createIndex error: %v", err)
	}

	// if we're here, the connected server does not support the command, so we fall back
	log.Log(log.Info, "\tcreateIndexes command not supported, attemping legacy index insertion")
	for _, idx := range indexes {
		log.Logf(log.Info, "\tmanually creating index %v", idx.Options["name"])
		err = restore.LegacyInsertIndex(intent, idx)
		if err != nil {
			return fmt.Errorf("error creating index %v: %v", idx.Options["name"], err)
		}
	}
	return nil
}

// LegacyInsertIndex takes in an intent and an index document and attempts to
// create the index on the "system.indexes" collection.
func (restore *MongoRestore) LegacyInsertIndex(intent *intents.Intent, index IndexDocument) error {
	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error establishing connection: %v", err)
	}
	defer session.Close()

	// overwrite safety to make sure we catch errors
	session.SetSafe(&mgo.Safe{})
	indexCollection := session.DB(intent.DB).C("system.indexes")
	err = indexCollection.Insert(index)
	if err != nil {
		return fmt.Errorf("insert error: %v", err)
	}

	return nil
}

// CreateCollection creates the collection specified in the intent with the
// given options.
func (restore *MongoRestore) CreateCollection(intent *intents.Intent, options bson.D) error {
	jsonCommand, err := bsonutil.ConvertBSONValueToJSON(
		append(bson.D{{"create", intent.C}}, options...),
	)
	if err != nil {
		return err
	}

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error establishing connection: %v", err)
	}
	defer session.Close()

	res := bson.M{}
	err = session.DB(intent.DB).Run(jsonCommand, &res)
	if err != nil {
		return fmt.Errorf("error running create command: %v", err)
	}
	if util.IsFalsy(res["ok"]) {
		return fmt.Errorf("create command: %v", res["errmsg"])
	}
	return nil
}

// RestoreUsersOrRoles accepts a collection type (Users or Roles) and restores the intent
// in the appropriate collection.
func (restore *MongoRestore) RestoreUsersOrRoles(collectionType string, intent *intents.Intent) error {
	log.Logf(log.Always, "restoring %v from %v", collectionType, intent.BSONPath)

	if intent.Size == 0 {
		// MongoDB complains if we try and remove a non-existent collection, so we should
		// just skip auth collections with empty .bson files to avoid gnarly logic later on.
		log.Logf(log.Always, "%v file '%v' is empty; skipping %v restoration",
			collectionType, intent.BSONPath, collectionType)
		return nil
	}

	var tempCol, tempColCommandField string
	switch collectionType {
	case Users:
		tempCol = restore.tempUsersCol
		tempColCommandField = "tempUsersCollection"
	case Roles:
		tempCol = restore.tempRolesCol
		tempColCommandField = "tempRolesCollection"
	default:
		return fmt.Errorf("cannot use %v as a collection type in RestoreUsersOrRoles", collectionType)
	}

	rawFile, err := os.Open(intent.BSONPath)
	if err != nil {
		return fmt.Errorf("error reading index bson file %v: %v", intent.BSONPath, err)
	}
	bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(rawFile))
	defer bsonSource.Close()

	tempColExists, err := restore.CollectionExists(&intents.Intent{DB: "admin", C: tempCol})
	if err != nil {
		return err
	}
	if tempColExists {
		return fmt.Errorf("temporary collection admin.%v already exists. "+
			"Drop it or specify new temporary collections with --tempUsersColl "+
			"and --tempRolesColl", tempCol)
	}

	log.Logf(log.DebugLow, "restoring %v to temporary collection", collectionType)
	if _, err = restore.RestoreCollectionToDB("admin", tempCol, bsonSource, 0); err != nil {
		return fmt.Errorf("error restoring %v: %v", collectionType, err)
	}

	// make sure we always drop the temporary collection
	defer func() {
		session, err := restore.SessionProvider.GetSession()
		if err != nil {
			// logging errors here because this has no way of returning that doesn't mask other errors
			log.Logf(log.Always, "error establishing connection to drop temporary collection %v: %v", tempCol, err)
			return
		}
		defer session.Close()
		log.Logf(log.DebugHigh, "dropping temporary collection %v", tempCol)
		err = session.DB("admin").C(tempCol).DropCollection()
		if err != nil {
			log.Logf(log.Always, "error dropping temporary collection %v: %v", tempCol, err)
		}
	}()

	// If we are restoring a single database (--restoreDBUsersAndRoles), then the
	// target database will be that database, and the _mergeAuthzCollections command
	// will only restore users/roles of that database. If we are restoring the admin db or
	// doing a full restore, we tell the command to merge users/roles of all databases.
	userTargetDB := intent.DB
	if userTargetDB == "admin" {
		// _mergeAuthzCollections uses an empty db string as a sentinel for "all databases"
		userTargetDB = ""
	}

	// we have to manually convert mgo's safety to a writeconcern object
	writeConcern := bson.M{}
	if restore.safety == nil {
		writeConcern["w"] = 0
	} else {
		if restore.safety.WMode != "" {
			writeConcern["w"] = restore.safety.WMode
		} else {
			writeConcern["w"] = restore.safety.W
		}
	}

	command := bsonutil.MarshalD{
		{"_mergeAuthzCollections", 1},
		{tempColCommandField, "admin." + tempCol},
		{"drop", restore.OutputOptions.Drop},
		{"writeConcern", writeConcern},
		{"db", userTargetDB},
	}

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error establishing connection: %v", err)
	}
	defer session.Close()

	log.Logf(log.DebugLow, "merging %v from temp collection '%v'", collectionType, tempCol)
	res := bson.M{}
	err = session.Run(command, &res)
	if err != nil {
		return fmt.Errorf("error running merge command: %v", err)
	}
	if util.IsFalsy(res["ok"]) {
		return fmt.Errorf("_mergeAuthzCollections command: %v", res["errmsg"])
	}
	return nil
}

// GetDumpAuthVersion reads the admin.system.version collection in the dump directory
// to determine the authentication version of the files in the dump. If that collection is not
// present in the dump, we try to infer the authentication version based on its absence.
// Returns the authentication version number and any errors that occur.
func (restore *MongoRestore) GetDumpAuthVersion() (int, error) {
	// first handle the case where we have no auth version
	intent := restore.manager.AuthVersion()
	if intent == nil {
		if restore.InputOptions.RestoreDBUsersAndRoles {
			// If we are using --restoreDbUsersAndRoles, we cannot guarantee an
			// $admin.system.version collection from a 2.6 server,
			// so we can assume up to version 3.
			log.Logf(log.Always, "no system.version bson file found in '%v' database dump", restore.ToolOptions.DB)
			log.Log(log.Always, "warning: assuming users and roles collections are of auth version 3")
			log.Log(log.Always, "if users are from an earlier version of MongoDB, they may not restore properly")
			return 3, nil
		}
		log.Log(log.Info, "no system.version bson file found in dump")
		log.Log(log.Always, "assuming users in the dump directory are from <= 2.4 (auth version 1)")
		return 1, nil
	}
	rawFile, err := os.Open(intent.BSONPath)
	if err != nil {
		return 0, fmt.Errorf("error reading version bson file %v: %v", intent.BSONPath, err)
	}
	bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(rawFile))
	defer bsonSource.Close()

	versionDoc := struct {
		CurrentVersion int `bson:"currentVersion"`
	}{}
	bsonSource.Next(&versionDoc)
	if err = bsonSource.Err(); err != nil {
		return 0, fmt.Errorf("error reading version bson file %v: %v", intent.BSONPath, err)
	}
	authVersion := versionDoc.CurrentVersion
	if authVersion == 0 {
		// 0 is not a possible valid version number, so this can only indicate bad input
		return 0, fmt.Errorf("system.version bson file does not have 'currentVersion' field")
	}
	return authVersion, nil
}

// ValidateAuthVersions compares the authentication version of the dump files and the
// authentication version of the target server, and returns an error if the versions
// are incompatible.
func (restore *MongoRestore) ValidateAuthVersions() error {
	if restore.authVersions.Dump == 2 || restore.authVersions.Dump == 4 {
		return fmt.Errorf(
			"cannot restore users and roles from a dump file with auth version %v; "+
				"finish the upgrade or roll it back", restore.authVersions.Dump)
	}
	if restore.authVersions.Server == 2 || restore.authVersions.Server == 4 {
		return fmt.Errorf(
			"cannot restore users and roles to a server with auth version %v; "+
				"finish the upgrade or roll it back", restore.authVersions.Server)
	}
	switch restore.authVersions {
	case authVersionPair{3, 5}:
		log.Log(log.Info,
			"restoring users and roles of auth version 3 to a server of auth version 5")
	case authVersionPair{5, 5}:
		log.Log(log.Info,
			"restoring users and roles of auth version 5 to a server of auth version 5")
	case authVersionPair{3, 3}:
		log.Log(log.Info,
			"restoring users and roles of auth version 3 to a server of auth version 3")
	case authVersionPair{1, 1}:
		log.Log(log.Info,
			"restoring users and roles of auth version 1 to a server of auth version 1")
	case authVersionPair{1, 5}:
		return fmt.Errorf("cannot restore users of auth version 1 to a server of auth version 5")
	case authVersionPair{5, 3}:
		return fmt.Errorf("cannot restore users of auth version 5 to a server of auth version 3")
	case authVersionPair{1, 3}:
		log.Log(log.Info,
			"restoring users and roles of auth version 1 to a server of auth version 3")
		log.Log(log.Always,
			"users and roles will have to be updated with the authSchemaUpgrade command")
	case authVersionPair{5, 1}:
		fallthrough
	case authVersionPair{3, 1}:
		return fmt.Errorf(
			"cannot restore users and roles dump file >= auth version 3 to a server of auth version 1")
	default:
		return fmt.Errorf("invalid auth pair: dump=%v, server=%v",
			restore.authVersions.Dump, restore.authVersions.Server)
	}
	return nil

}

// ShouldRestoreUsersAndRoles returns true if mongorestore should go through
// through the process of restoring collections pertaining to authentication.
func (restore *MongoRestore) ShouldRestoreUsersAndRoles() bool {
	// If the user has done anything that would indicate the restoration
	// of users and roles (i.e. used --restoreDbUsersAndRoles, -d admin, or
	// is doing a full restore), then we check if users or roles BSON files
	// actually exist in the dump dir. If they do, return true.
	if restore.InputOptions.RestoreDBUsersAndRoles ||
		restore.ToolOptions.DB == "" ||
		restore.ToolOptions.DB == "admin" {
		if restore.manager.Users() != nil || restore.manager.Roles() != nil {
			return true
		}
	}
	return false
}

// DropCollection drops the intent's collection.
func (restore *MongoRestore) DropCollection(intent *intents.Intent) error {
	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error establishing connection: %v", err)
	}
	defer session.Close()
	err = session.DB(intent.DB).C(intent.C).DropCollection()
	if err != nil {
		return fmt.Errorf("error dropping collection: %v", err)
	}
	return nil
}

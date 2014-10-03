package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"os"
	"strings"
)

const Users = "users"
const Roles = "roles"

type Metadata struct {
	Options bson.D          `json:"options,omitempty"`
	Indexes []IndexDocument `json:"indexes"`
}

// this struct is used to read in the options of a set of indexes
type metaDataMapIndex struct {
	Indexes []bson.M `json:"indexes"`
}

type IndexDocument struct {
	Options bson.M `bson:",inline"`
	Key     bson.D `bson:"key"`
}

// MetadataFromJSON takes a slice of JSON bytes and unmarshals them into usable
// collection options and indexes for restoring collections.
func (restore *MongoRestore) MetadataFromJSON(jsonBytes []byte) (bson.D, []IndexDocument, error) {
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
		// remove "key" and "v" from the map versions
		delete(metaAsMap.Indexes[i], "key")
		meta.Indexes[i].Options = metaAsMap.Indexes[i]
	}

	return meta.Options, meta.Indexes, nil
}

//TODO test this
func (restore *MongoRestore) IndexesFromBSON(intent *Intent, bsonFile string) ([]IndexDocument, error) {
	log.Logf(2, "scanning %v for indexes on %v collections", bsonFile, intent.C)

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
			log.Logf(3, "\tfound index %v", indexDocument.Options["name"])
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

func (restore *MongoRestore) DBHasCollection(intent *Intent) (bool, error) {
	collectionNS := intent.Key()
	result := bson.M{}
	err := restore.cmdRunner.FindOne(intent.DB, "system.namespaces", 0, bson.M{"name": collectionNS}, nil, &result, 0)
	if err != nil {
		// handle case when using db connection
		if err == mgo.ErrNotFound {
			log.Logf(3, "collection %v does not exists", collectionNS)
			return false, nil
		}
		return false, err
	}
	if len(result) > 0 {
		log.Logf(3, "collection %v already exists", collectionNS)
		return true, nil
	}
	log.Logf(3, "collection %v does not exists", collectionNS)
	return false, nil
}

func (restore *MongoRestore) InsertIndex(intent *Intent, index IndexDocument) error {
	// first, update the namespace of the index before inserting
	index.Options["ns"] = intent.Key()

	// remove the index version, forcing an update,
	// unless we specifically want to keey it
	if !restore.OutputOptions.KeepIndexVersion {
		delete(index.Options, "v")
	}

	// overwrite safety to make sure we catch errors
	insertStream, err := restore.cmdRunner.OpenInsertStream(intent.DB, "system.indexes", &mgo.Safe{})
	if err != nil {
		return fmt.Errorf("error opening insert connection: %v", err)
	}
	defer insertStream.Close()
	err = insertStream.WriteDoc(index)
	if err != nil {
		return fmt.Errorf("insert error: %v", err)
		//TODO, more error checking? Audit this...
	}

	return nil
}

func (restore *MongoRestore) CreateCollection(intent *Intent, options bson.D) error {
	jsonCommand, err := bsonutil.ConvertBSONValueToJSON(
		append(bson.D{{"create", intent.C}}, options...),
	)
	if err != nil {
		return err
	}

	res := bson.M{}
	err = restore.cmdRunner.Run(jsonCommand, &res, intent.DB)
	if err != nil {
		return fmt.Errorf("error running create command: %v", err)
	}
	if util.IsFalsy(res["ok"]) {
		return fmt.Errorf("create command: %v", res["errmsg"])
	}
	return nil
}

func (restore *MongoRestore) RestoreUsersOrRoles(collectionType string, intent *Intent) error {
	log.Logf(0, "restoring %v from %v", collectionType, intent.BSONPath)

	var tempCol, tempColCommandField, collectionName string
	switch collectionType {
	case Users:
		tempCol = restore.tempUsersCol
		tempColCommandField = "tempUsersCollection"
		collectionName = "system.users"
	case Roles:
		tempCol = restore.tempRolesCol
		tempColCommandField = "tempRolesCollection"
		collectionName = "system.roles"
	default:
		// panic should be fine here, since this is a programmer (not user) error
		util.Panicf("cannot use %v as a collection type in RestoreUsersOrRoles", collectionType)
	}

	rawFile, err := os.Open(intent.BSONPath)
	if err != nil {
		return fmt.Errorf("error reading index bson file %v: %v", intent.BSONPath, err)
	}
	bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(rawFile))
	defer bsonSource.Close()

	// branch here, using a temporary collection if the db is live and simple inserts if it's the shim
	// TODO nicer encapsulation of logic
	if _, ok := restore.cmdRunner.(*db.Shim); ok {
		log.Logf(2, "using the shim, restoring %v directly", collectionType)

		// first, drop all db-related users/roles if drop is enabled
		if restore.OutputOptions.Drop {
			if intent.DB != "admin" {
				log.Logf(2, "removing all %v for database %v from admin db", collectionType, intent.DB)
				err := restore.cmdRunner.Remove("admin", collectionName, bson.M{"db": intent.DB})
				if err != nil {
					return fmt.Errorf("error dropping all %v for database %v: %v", collectionType, intent.DB, err)
				}
			} else {
				log.Logf(2, "removing all %v from admin db", collectionType)
				err := restore.cmdRunner.Remove("admin", collectionName, bson.M{})
				if err != nil {
					return fmt.Errorf("error dropping all %v from admin db", collectionType, intent.DB, err)
				}
			}
		}

		// then iterate through all entries, updating their "db" and inserting them
		insertStream, err := restore.cmdRunner.OpenInsertStream("admin", collectionName, restore.safety)
		if err != nil {
			return fmt.Errorf("error opening insert connection: %v", err)
		}
		defer insertStream.Close()
		doc := bson.M{}

		log.Logf(0, "restoring %v for database %v into admin.%v", collectionType, intent.DB, collectionName)
		for bsonSource.Next(&doc) {
			if restore.ToolOptions.DB != "" && intent.DB != "admin" {
				// update the db field if we're restoring with a db option
				// TODO: confirm we should do this?
				doc["db"] = restore.ToolOptions.DB
			}
			err := insertStream.WriteDoc(doc)
			if err != nil {
				return fmt.Errorf("error inserting %v: %v", collectionType, err)
			}
		}
		if bsonSource.Err() != nil {
			return fmt.Errorf("error reading %v: %v", intent.BSONPath, bsonSource.Err())
		}

		return nil
	}

	tempColExists, err := restore.DBHasCollection(&Intent{DB: "admin", C: tempCol})
	if err != nil {
		return err
	}
	if tempColExists {
		return fmt.Errorf("temporary collection admin.%v already exists", tempCol) //TODO(erf) make this more helpful
	}

	log.Logf(2, "restoring %v to temporary collection", collectionType)
	err = restore.RestoreCollectionToDB("admin", tempCol, bsonSource, 0)
	if err != nil {
		return fmt.Errorf("error restoring %v: %v", collectionType, err)
	}

	// make sure we always drop the temporary collection
	defer func() {
		log.Logf(3, "dropping temporary collection %v", tempCol) //TODO(erf) increase verbosity here
		err = restore.cmdRunner.Run(bson.M{"drop": tempCol}, &bson.M{}, "admin")
		if err != nil {
			log.Logf(0, "error dropping temporary collection %v: %v", tempCol, err)
		}
	}()

	userTargetDB := intent.DB
	// use "admin" as the merge db unless we are restoring admin
	if restore.ToolOptions.DB == "admin" {
		userTargetDB = ""
	}

	command := bsonutil.MarshalD{
		{"_mergeAuthzCollections", 1},
		{tempColCommandField, "admin." + tempCol},
		{"drop", restore.OutputOptions.Drop},
		{"writeConcern", bson.M{"w": restore.OutputOptions.WriteConcern}},
		{"db", userTargetDB},
	}

	log.Logf(2, "merging %v from temp collection", collectionType)
	res := bson.M{}
	err = restore.cmdRunner.Run(command, &res, "admin")
	if err != nil {
		return fmt.Errorf("error running merge command: %v", err)
	}
	if util.IsFalsy(res["ok"]) {
		return fmt.Errorf("_mergeAuthzCollections command: %v", res["errmsg"])
	}
	return nil

}

package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

//TODO make this common
type Metadata struct {
	Options bson.D          `json:"options,omitempty"`
	Indexes []IndexDocument `json:"indexes"`
}

type MetaDataMapIndex struct {
	Indexes []bson.M `json:"indexes"`
}

type IndexDocument struct {
	Options bson.M `bson:",inline"`
	Key     bson.D `bson:"key"`
}

func (restore *MongoRestore) MetadataFromJSON(jsonBytes []byte) (bson.D, []IndexDocument, error) {
	meta := &Metadata{}
	err := json.Unmarshal(jsonBytes, meta)
	if err != nil {
		return nil, nil, err
	}

	// first get the ordered key information for each index,
	// then merge it with a set of options stored as a map
	metaAsMap := MetaDataMapIndex{}
	err = json.Unmarshal(jsonBytes, &metaAsMap)
	if err != nil {
		return nil, nil, fmt.Errorf("error unmarshalling metadata as map: %v", err)
	}
	for i := range meta.Indexes {
		// remove "key" and "v" from the map versions
		delete(metaAsMap.Indexes[i], "key")
		if !restore.OutputOptions.KeepIndexVersion {
			delete(metaAsMap.Indexes[i], "v")
		}
		meta.Indexes[i].Options = metaAsMap.Indexes[i]
	}

	log.Logf(0, "%#v", meta.Options)

	return meta.Options, meta.Indexes, nil
}

func (restore *MongoRestore) DBHasCollection(intent *Intent) (bool, error) {
	collectionNS := intent.Key()
	err := restore.cmdRunner.FindOne(intent.DB, "system.namespaces", 0, bson.M{"name": collectionNS}, nil, &bson.M{}, 0)
	if err != nil {
		if err == mgo.ErrNotFound {
			log.Logf(3, "collection %v does not exists", collectionNS)
			return false, nil
		}
		return false, err
	}
	log.Logf(3, "collection %v already exists", collectionNS)
	return true, nil
}

func (restore *MongoRestore) InsertIndex(dbName string, index IndexDocument) error {
	// overwrite safety to make sure we catch errors
	insertStream, err := restore.cmdRunner.OpenInsertStream(dbName, "system.indexes", &mgo.Safe{})
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

package db

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"strings"
)

func IsNoCmd(err error) bool {
	e, ok := err.(*mgo.QueryError)
	return ok && strings.HasPrefix(e.Message, "no such cmd:")
}

//buildBsonArray takes a cursor iterator and returns an array of
//all of its documents as bson.D objects.
func buildBsonArray(iter *mgo.Iter) ([]bson.D, error) {
	ret := make([]bson.D, 0, 0)
	index := new(bson.D)
	for iter.Next(index) {
		ret = append(ret, *index)
		index = new(bson.D)
	}

	if iter.Err() != nil {
		return nil, iter.Err()
	}
	return ret, nil

}

//GetIndexes is a helper function that gets the raw index info for a particular
//collection by using the listIndexes command if available, or by falling back
//to querying against system.indexes (pre-2.8 systems)
func GetIndexes(coll *mgo.Collection) ([]bson.D, error) {
	var cmdResult struct {
		Indexes []bson.D
	}

	err := coll.Database.Run(bson.M{"listIndexes": coll.Name}, &cmdResult)
	switch {
	case err == nil:
		return cmdResult.Indexes, nil
	case IsNoCmd(err):
		log.Logf(log.DebugLow, "No support for listIndexes command, falling back to querying system.indexes")
		return getIndexesPre28(coll)
	default:
		return nil, fmt.Errorf("error running `listIndexes`. Collection: `%v` Err: %v", coll.FullName, err)
	}
}

func getIndexesPre28(coll *mgo.Collection) ([]bson.D, error) {
	indexColl := coll.Database.C("system.indexes")
	iter := indexColl.Find(&bson.M{"ns": coll.FullName}).Iter()
	ret, err := buildBsonArray(iter)
	if err != nil {
		return nil, fmt.Errorf("error iterating through collection. Collection: `%v` Err: %v",
			coll.FullName, err)
	}

	return ret, nil
}

func GetCollections(database *mgo.Database) ([]bson.D, error) {
	var cmdResult struct {
		Collections []bson.D
	}

	err := database.Run(bson.M{"listCollections": 1}, &cmdResult)
	switch {
	case err == nil:
		return cmdResult.Collections, nil
	case IsNoCmd(err):
		log.Logf(log.DebugLow, "No support for listCollections command, falling back to querying system.namespaces")
		return getCollectionsPre28(database)
	default:
		return nil, fmt.Errorf("error running `listCollections`. Database: `%v` Err: %v",
			database.Name, err)
	}
}

func getCollectionsPre28(database *mgo.Database) ([]bson.D, error) {
	indexColl := database.C("system.namespaces")
	iter := indexColl.Find(&bson.M{}).Iter()
	ret, err := buildBsonArray(iter)
	if err != nil {
		return nil, fmt.Errorf("error iterating through namespaces. Collection: `%v` Err: %v",
			indexColl.FullName, err)
	}

	return ret, nil
}

func GetCollectionOptions(coll *mgo.Collection) (*bson.D, error) {
	var cmdResult struct {
		Collections []bson.D
	}
	err := coll.Database.Run(bson.M{"listCollections": 1}, &cmdResult)
	switch {
	case err == nil:
		for _, collectionInfo := range cmdResult.Collections {
			name, err := bsonutil.FindValueByKey("name", &collectionInfo)
			if err != nil {
				continue
			}
			if nameStr, ok := name.(string); ok {
				if nameStr == coll.Name {
					return &collectionInfo, nil
				}
			} else {
				continue
			}
		}
		// The given collection was not found, but no error encountered.
		return nil, nil
	case IsNoCmd(err):
		collInfo := &bson.D{}
		namespacesColl := coll.Database.C("system.namespaces")
		err = namespacesColl.Find(&bson.M{"name": coll.FullName}).One(collInfo)
		if err != nil {
			return nil, err
		}
		return collInfo, nil
	default:
		return nil, err
	}
}

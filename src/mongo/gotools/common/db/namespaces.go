package db

import (
	"fmt"
	"strings"

	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

type CollectionInfo struct {
	Name    string  `bson:"name"`
	Type    string  `bson:"type"`
	Options *bson.D `bson:"options"`
}

func (ci *CollectionInfo) IsView() bool {
	return ci.Type == "view"
}

// IsNoCmd reeturns true if err indicates a query command is not supported,
// otherwise, returns false.
func IsNoCmd(err error) bool {
	e, ok := err.(*mgo.QueryError)
	return ok && strings.HasPrefix(e.Message, "no such cmd:")
}

// IsNoCollection returns true if err indicates a query resulted in a "no collection" error
// otherwise, returns false.
func IsNoCollection(err error) bool {
	e, ok := err.(*mgo.QueryError)
	return ok && e.Message == "no collection"
}

// buildBsonArray takes a cursor iterator and returns an array of
// all of its documents as bson.D objects.
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

// GetIndexes returns an iterator to thethe raw index info for a collection by
// using the listIndexes command if available, or by falling back to querying
// against system.indexes (pre-3.0 systems). nil is returned if the collection
// does not exist.
func GetIndexes(coll *mgo.Collection) (*mgo.Iter, error) {
	var cmdResult struct {
		Cursor struct {
			FirstBatch []bson.Raw `bson:"firstBatch"`
			NS         string
			Id         int64
		}
	}

	err := coll.Database.Run(bson.D{{"listIndexes", coll.Name}, {"cursor", bson.M{}}}, &cmdResult)
	switch {
	case err == nil:
		ns := strings.SplitN(cmdResult.Cursor.NS, ".", 2)
		if len(ns) < 2 {
			return nil, fmt.Errorf("server returned invalid cursor.ns `%v` on listIndexes for `%v`: %v",
				cmdResult.Cursor.NS, coll.FullName, err)
		}

		ses := coll.Database.Session
		return ses.DB(ns[0]).C(ns[1]).NewIter(ses, cmdResult.Cursor.FirstBatch, cmdResult.Cursor.Id, nil), nil
	case IsNoCmd(err):
		log.Logvf(log.DebugLow, "No support for listIndexes command, falling back to querying system.indexes")
		return getIndexesPre28(coll)
	case IsNoCollection(err):
		return nil, nil
	default:
		return nil, fmt.Errorf("error running `listIndexes`. Collection: `%v` Err: %v", coll.FullName, err)
	}
}

func getIndexesPre28(coll *mgo.Collection) (*mgo.Iter, error) {
	indexColl := coll.Database.C("system.indexes")
	iter := indexColl.Find(&bson.M{"ns": coll.FullName}).Iter()
	return iter, nil
}

func GetCollections(database *mgo.Database, name string) (*mgo.Iter, bool, error) {
	var cmdResult struct {
		Cursor struct {
			FirstBatch []bson.Raw `bson:"firstBatch"`
			NS         string
			Id         int64
		}
	}

	command := bson.D{{"listCollections", 1}, {"cursor", bson.M{}}}
	if len(name) > 0 {
		command = bson.D{{"listCollections", 1}, {"filter", bson.M{"name": name}}, {"cursor", bson.M{}}}
	}

	err := database.Run(command, &cmdResult)
	switch {
	case err == nil:
		ns := strings.SplitN(cmdResult.Cursor.NS, ".", 2)
		if len(ns) < 2 {
			return nil, false, fmt.Errorf("server returned invalid cursor.ns `%v` on listCollections for `%v`: %v",
				cmdResult.Cursor.NS, database.Name, err)
		}

		return database.Session.DB(ns[0]).C(ns[1]).NewIter(database.Session, cmdResult.Cursor.FirstBatch, cmdResult.Cursor.Id, nil), false, nil
	case IsNoCmd(err):
		log.Logvf(log.DebugLow, "No support for listCollections command, falling back to querying system.namespaces")
		iter, err := getCollectionsPre28(database, name)
		return iter, true, err
	default:
		return nil, false, fmt.Errorf("error running `listCollections`. Database: `%v` Err: %v",
			database.Name, err)
	}
}

func getCollectionsPre28(database *mgo.Database, name string) (*mgo.Iter, error) {
	indexColl := database.C("system.namespaces")
	selector := bson.M{}
	if len(name) > 0 {
		selector["name"] = database.Name + "." + name
	}
	iter := indexColl.Find(selector).Iter()
	return iter, nil
}

func GetCollectionInfo(coll *mgo.Collection) (*CollectionInfo, error) {
	iter, useFullName, err := GetCollections(coll.Database, coll.Name)
	if err != nil {
		return nil, err
	}
	defer iter.Close()
	comparisonName := coll.Name
	if useFullName {
		comparisonName = coll.FullName
	}

	collInfo := &CollectionInfo{}
	for iter.Next(collInfo) {
		if collInfo.Name == comparisonName {
			if useFullName {
				collName, err := StripDBFromNamespace(collInfo.Name, coll.Database.Name)
				if err != nil {
					return nil, err
				}
				collInfo.Name = collName
			}
			break
		}
	}
	if err := iter.Err(); err != nil {
		return nil, err
	}
	return collInfo, nil
}

func StripDBFromNamespace(namespace string, dbName string) (string, error) {
	namespacePrefix := dbName + "."
	// if the collection info came from querying system.indexes (2.6 or earlier) then the
	// "name" we get includes the db name as well, so we must remove it
	if strings.HasPrefix(namespace, namespacePrefix) {
		return namespace[len(namespacePrefix):], nil
	}
	return "", fmt.Errorf("namespace '%v' format is invalid - expected to start with '%v'", namespace, namespacePrefix)
}

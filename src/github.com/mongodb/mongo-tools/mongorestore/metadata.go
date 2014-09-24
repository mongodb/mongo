package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

//TODO make this common
type Metadata struct {
	Options bson.M          `json:"options,omitempty"`
	Indexes []IndexDocument `json:"indexes"`
}

type IndexDocument struct {
	Name      string `bson:"name"`
	Namespace string `bson:"ns"`
	Value     int    `bson:"v"`
	Key       bson.D `bson:"key"`

	Unique     bool `bson:"unique"`
	DropDups   bool `bson:"dropDups"`
	Background bool `bson:"background"`
	Sparse     bool `bson:"sparse"`

	//TODO expire, etc
}

func MetadataFromJSON(jsonBytes []byte) (*mgo.CollectionInfo, []mgo.Index, error) {
	meta := &Metadata{}
	err := json.Unmarshal(jsonBytes, meta)
	if err != nil {
		return nil, nil, err
	}

	var colInfo *mgo.CollectionInfo
	if len(meta.Options) > 0 {
		colInfo, err = CollectionInfoFromOptions(meta.Options)
		if err != nil {
			return nil, nil, fmt.Errorf("error reading options: %v", err)
		}
	}

	newIndexes := []mgo.Index{}
	for _, idx := range meta.Indexes {
		newIndex, err := idx.ToMgoFormat()
		if err != nil {
			return nil, nil, fmt.Errorf("error reading indexes: %v", err)
		}
		newIndexes = append(newIndexes, newIndex)
	}

	return colInfo, newIndexes, nil
}

func CollectionInfoFromOptions(in bson.M) (*mgo.CollectionInfo, error) {
	info := &mgo.CollectionInfo{}
	if cappedVal, ok := in["capped"]; ok {
		info.Capped = util.IsTruthy(cappedVal)
		if sizeVal, ok := in["size"]; ok {
			var err error
			info.MaxBytes, err = util.ToInt(sizeVal)
			if err != nil {
				return nil, fmt.Errorf("bad 'size' value: %v", err)
			}
			if maxVal, ok := in["max"]; ok {
				var err error
				info.MaxDocs, err = util.ToInt(maxVal)
				if err != nil {
					return nil, fmt.Errorf("bad 'max' value: %v", err)
				}
			}
		}
	}

	if autoVal, ok := in["autoIndexId"]; ok {
		if util.IsTruthy(autoVal) {
			info.ForceIdIndex = true
		} else {
			info.DisableIdIndex = true
		}
	}

	//TODO gustavo re: powerOf2! Or just hack it here
	return info, nil

}

func (idx IndexDocument) ToMgoFormat() (mgo.Index, error) {
	mgoIdx := mgo.Index{}
	for _, elem := range idx.Key {
		keyField := elem.Name
		keyInt, err := util.ToInt(elem.Value)
		if err != nil {
			return mgoIdx, err
		}
		newKeyVal := "" //TODO, is this logic safe??
		if keyInt > 0 {
			newKeyVal = "+" + keyField //TODO SUPPORT HASHED INDEX
		} else {
			newKeyVal = "-" + keyField
		}
		mgoIdx.Key = append(mgoIdx.Key, newKeyVal)
	}
	mgoIdx.Name = idx.Name
	mgoIdx.Unique = idx.Unique
	mgoIdx.DropDups = idx.DropDups
	mgoIdx.Background = idx.Background
	mgoIdx.Sparse = idx.Sparse
	//TODO the rest of it..., stop using MGO here
	return mgoIdx, nil
}

func DBHasCollection(db *mgo.Database, collectionNS string) (bool, error) {
	err := db.C("system.namespaces").Find(bson.M{"name": collectionNS}).One(&bson.M{})
	if err != nil {
		if err == mgo.ErrNotFound {
			log.Logf(3, "collection %v already exists", collectionNS)
			return false, nil
		}
		return false, err
	}
	log.Logf(3, "collection %v does not exists", collectionNS)
	return true, nil
}

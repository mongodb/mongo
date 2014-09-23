package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"strings"
)

type ImportWriter interface {
	Open(db, collection string) error
	Import(doc bson.M) error
	Close() error
	Drop() error
}

type DriverImportWriter struct {
	upsertMode      bool
	upsertFields    []string
	sessionProvider *db.SessionProvider
	session         *mgo.Session
	collection      *mgo.Collection //cached
}

type ShimImportWriter struct {
	upsertMode   bool
	upsertFields []string

	importShim *db.StorageShim //shim used during the actual import phase itself
	docSink    *db.EncodedBSONSink

	dbPath             string
	dbName, collection string
	shimPath           string
}

func (siw *ShimImportWriter) Open(dbName, collection string) error {
	siw.dbName = dbName
	siw.collection = collection
	shimPath, err := db.LocateShim()
	if err != nil {
		return err
	}
	siw.shimPath = shimPath
	return nil
}

func (siw *ShimImportWriter) Drop() error {
	dropShim := db.StorageShim{
		DBPath:     siw.dbPath,
		Database:   siw.dbName,
		Collection: siw.collection,
		ShimPath:   siw.shimPath,
		Query:      "{}",
		Mode:       db.Drop,
	}
	_, _, err := dropShim.Open()
	if err != nil {
		return err
	}

	/*
		decodedResult := db.NewDecodedBSONSource(out)
		resultDoc := bson.M{}
		decodedResult.Next(&resultDoc)
	*/

	defer dropShim.Close()
	return dropShim.WaitResult()
}

func (siw *ShimImportWriter) initImportShim() error {
	importShim := &db.StorageShim{
		DBPath:     siw.dbPath,
		Database:   siw.dbName,
		Collection: siw.collection,
		ShimPath:   siw.shimPath,
		Query:      "",
		Mode:       db.Insert,
	}
	_, inStream, err := importShim.Open()
	if err != nil {
		return err
	}
	siw.importShim = importShim
	siw.docSink = &db.EncodedBSONSink{inStream}
	return nil
}

func (siw *ShimImportWriter) Import(doc bson.M) error {
	if siw.importShim == nil {
		//lazily initialize import shim
		err := siw.initImportShim()
		if err != nil {
			return err
		}
	}
	_, err := siw.docSink.WriteDoc(doc)
	if err != nil {
		return err
	}
	return nil
}

func (siw *ShimImportWriter) Close() error {
	if siw.importShim != nil {
		return siw.importShim.Close()
	}
	return nil
}

func (diw *DriverImportWriter) Open(db, collection string) error {
	if diw.session != nil {
		panic("import writer already open")
	}
	session, err := diw.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	diw.session = session
	diw.collection = session.DB(db).C(collection)
	return nil
}

func (diw *DriverImportWriter) Drop() error {
	if diw.session == nil {
		panic("import writer not open")
	}
	return diw.collection.DropCollection()
}

// constructUpsertDocument constructs a BSON document to use for upserts
func constructUpsertDocument(upsertFields []string, document bson.M) bson.M {
	upsertDocument := bson.M{}
	var hasDocumentKey bool
	for _, key := range upsertFields {
		upsertDocument[key] = getUpsertValue(key, document)
		if upsertDocument[key] != nil {
			hasDocumentKey = true
		}
	}
	if !hasDocumentKey {
		return nil
	}
	return upsertDocument
}

// getUpsertValue takes a given BSON document and a given field, and returns the
// field's associated value in the document. The field is specified using dot
// notation for nested fields. e.g. "person.age" would return 34 would return
// 34 in the document: bson.M{"person": bson.M{"age": 34}} whereas,
// "person.name" would return nil
func getUpsertValue(field string, document bson.M) interface{} {
	index := strings.Index(field, ".")
	if index == -1 {
		return document[field]
	}
	left := field[0:index]
	if document[left] == nil {
		return nil
	}
	subDoc, ok := document[left].(bson.M)
	if !ok {
		return nil
	}
	return getUpsertValue(field[index+1:], subDoc)
}

func (diw *DriverImportWriter) Import(doc bson.M) error {
	if diw.upsertMode {
		selector := constructUpsertDocument(diw.upsertFields, doc)
		if selector == nil {
			return diw.collection.Insert(doc)
		}
		_, err := diw.collection.Upsert(selector, doc)
		return err
	}
	return diw.collection.Insert(doc)
}

func (diw *DriverImportWriter) Close() error {
	if diw.session != nil {
		diw.session.Close()
	}
	return nil
}

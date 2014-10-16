package mongoimport

import (
	"errors"
	"github.com/mongodb/mongo-tools/common/db"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"strings"
)

var errNoReachableServer = errors.New("no reachable servers")

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
	importShim   *db.StorageShim
	docSink      *db.EncodedBSONSink
	dbPath       string
	dirPerDB     bool
	db           string
	collection   string
	shimPath     string
}

// ShimImportWriter
func (siw *ShimImportWriter) initImportShim(upsert bool) error {
	mode := db.Insert
	if upsert {
		mode = db.Upsert
	}
	importShim := &db.StorageShim{
		DBPath:         siw.dbPath,
		DirectoryPerDB: siw.dirPerDB,
		Database:       siw.db,
		Collection:     siw.collection,
		ShimPath:       siw.shimPath,
		Query:          "",
		Mode:           mode,
		UpsertFields:   strings.Join(siw.upsertFields, ","),
	}
	_, inStream, err := importShim.Open()
	if err != nil {
		return err
	}
	siw.importShim = importShim
	siw.docSink = &db.EncodedBSONSink{
		BSONIn:     inStream,
		WriterShim: importShim,
	}
	return nil
}

func (siw *ShimImportWriter) Open(dbName, collection string) error {
	siw.db = dbName
	siw.collection = collection
	shimPath, err := db.LocateShim()
	if err != nil {
		return err
	}
	siw.shimPath = shimPath
	return nil
}

func (siw *ShimImportWriter) Import(doc bson.M) error {
	if siw.importShim == nil {
		// lazily initialize import shim
		if err := siw.initImportShim(siw.upsertMode); err != nil {
			return err
		}
	}
	return siw.docSink.WriteDoc(doc)
}

func (siw *ShimImportWriter) Drop() error {
	dropShim := db.StorageShim{
		DBPath:         siw.dbPath,
		DirectoryPerDB: siw.dirPerDB,
		Database:       siw.db,
		Collection:     siw.collection,
		ShimPath:       siw.shimPath,
		Query:          "{}",
		Mode:           db.Drop,
	}
	_, _, err := dropShim.Open()
	if err != nil {
		return err
	}
	defer dropShim.Close()
	return dropShim.WaitResult()
}

func (siw *ShimImportWriter) Close() error {
	if siw.importShim != nil {
		return siw.importShim.Close()
	}
	return nil
}

// DriverImportWriter
func (diw *DriverImportWriter) Open(db, collection string) error {
	if diw.session != nil {
		panic("import writer already open")
	}
	session, err := diw.sessionProvider.GetSession()
	if err != nil {
		return err
	}
	diw.session = session
	session.SetSocketTimeout(0)
	diw.collection = session.DB(db).C(collection)
	return nil
}

func (diw *DriverImportWriter) Drop() error {
	if diw.session == nil {
		panic("import writer not open")
	}
	return diw.collection.DropCollection()
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

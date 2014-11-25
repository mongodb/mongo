package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"io/ioutil"
	"os"
	"path/filepath"
	"strings"
)

// Filename helpers

type FileType uint

const (
	UnknownFileType FileType = iota
	BSONFileType
	MetadataFileType
)

// GetInfoFromFilename pulls the base collection name and
// type of file from a .bson/.metadata.json file
func GetInfoFromFilename(filename string) (string, FileType) {
	baseFileName := filepath.Base(filename)
	switch {
	case strings.HasSuffix(baseFileName, ".metadata.json"):
		// this logic can't be simple because technically
		// "x.metadata.json" is a valid collection name
		baseName := strings.TrimSuffix(baseFileName, ".metadata.json")
		return baseName, MetadataFileType
	case strings.HasSuffix(baseFileName, ".bin"):
		// .bin supported for legacy reasons
		// TODO: should we do this?
		baseName := strings.TrimSuffix(baseFileName, ".bin")
		return baseName, BSONFileType
	case strings.HasSuffix(baseFileName, ".bson"):
		baseName := strings.TrimSuffix(baseFileName, ".bson")
		return baseName, BSONFileType
	default:
		return "", UnknownFileType
	}
}

func (restore *MongoRestore) CreateAllIntents(fullpath string) error {
	log.Logf(log.DebugHigh, "using %v as dump root directory", fullpath)
	foundOplog := false
	entries, err := readDirWithSymlinks(fullpath)
	if err != nil {
		return fmt.Errorf("error reading root dump folder: %v", err)
	}
	for _, entry := range entries {
		if entry.IsDir() {
			if err = util.ValidateDBName(entry.Name()); err != nil {
				return fmt.Errorf("invalid database name '%v': %v", entry.Name(), err)
			}
			err = restore.CreateIntentsForDB(entry.Name(), filepath.Join(fullpath, entry.Name()))
			if err != nil {
				return err
			}
		} else {
			if entry.Name() == "oplog.bson" {
				if restore.InputOptions.OplogReplay {
					log.Log(log.DebugLow, "found oplog.bson file to replay")
				}
				foundOplog = true
				restore.manager.Put(&intents.Intent{
					C:        "oplog", //TODO make this a helper in intent
					BSONPath: filepath.Join(fullpath, entry.Name()),
					Size:     entry.Size(),
				})
			} else {
				log.Logf(log.Always, `don't know what to do with file "%v", skipping...`,
					filepath.Join(fullpath, entry.Name()))
			}
		}
	}
	if restore.InputOptions.OplogReplay && !foundOplog {
		return fmt.Errorf(
			"no %v/oplog.bson file to replay; make sure you run mongodump with --oplog.",
			fullpath)
	}
	return nil
}

// CreateIntentsForDB drills down into a db folder, creating intents
// for all of the collection dump files it encounters
func (restore *MongoRestore) CreateIntentsForDB(db, fullpath string) error {

	log.Logf(log.DebugHigh, "reading collections for database %v in %v", db, fullpath)
	entries, err := ioutil.ReadDir(fullpath)
	if err != nil {
		return fmt.Errorf("error reading db folder %v: %v", db, err)
	}
	//TODO check if we still want to even deal with this
	usesMetadataFiles := hasMetadataFiles(entries)
	for _, entry := range entries {
		if entry.IsDir() {
			log.Logf(log.Always, `don't know what to do with subdirectory "%v", skipping...`,
				filepath.Join(fullpath, entry.Name()))
		} else {
			//TODO handle user/roles?
			collection, fileType := GetInfoFromFilename(entry.Name())
			switch fileType {
			case BSONFileType:
				// skip restoring the indexes collection if we are using metadata
				// files to store index information, to eliminate redundancy
				if collection == "system.indexes" && usesMetadataFiles {
					log.Logf(log.DebugLow,
						"not restoring system.indexes collection because database %v "+
							"has .metadata.json files", db)
					continue
				}
				intent := &intents.Intent{
					DB:       db,
					C:        collection,
					Size:     entry.Size(),
					BSONPath: filepath.Join(fullpath, entry.Name()),
				}
				log.Logf(log.Info, "found collection %v bson to restore", intent.Key())
				restore.manager.Put(intent)
			case MetadataFileType:
				usesMetadataFiles = true
				intent := &intents.Intent{
					DB:           db,
					C:            collection,
					MetadataPath: filepath.Join(fullpath, entry.Name()),
				}
				log.Logf(log.Info, "found collection %v metadata to restore", intent.Key())
				restore.manager.Put(intent)
			default:
				log.Logf(log.Always, `don't know what to do with file "%v", skipping...`,
					filepath.Join(fullpath, entry.Name()))
			}
		}
	}
	return nil
}

// helper for searching a list of FileInfo for metadata files
func hasMetadataFiles(files []os.FileInfo) bool {
	for _, file := range files {
		if strings.HasSuffix(file.Name(), ".metadata.json") {
			return true
		}
	}
	return false
}

// CreateIntentForCollection builds an intent for the given db and collection name
// along with a path to a .bson collection file. It searches the .bson's directory
// for a matching metadata file. This method is not called by CreateIntentsForDB,
// it is only used in the case where --db and --collection flags are set.
func (restore *MongoRestore) CreateIntentForCollection(
	db, collection, fullpath string) error {

	log.Logf(log.DebugLow, "reading collection %v for database %v from %v",
		collection, db, fullpath)

	// avoid actual file handling if we are using stdin
	// TODO encapsulate this is a cleaner way
	if restore.useStdin {
		intent := &intents.Intent{
			DB:       db,
			C:        collection,
			BSONPath: "-",
		}
		restore.manager.Put(intent)
		return nil
	}

	// first make sure the bson file exists and is valid
	file, err := os.Lstat(fullpath)
	if err != nil {
		return err
	}
	if file.IsDir() {
		return fmt.Errorf("file %v is a directory, not a bson file", fullpath)
	}

	baseName, fileType := GetInfoFromFilename(file.Name())
	if fileType != BSONFileType {
		return fmt.Errorf("file %v does not have .bson extension", fullpath)
	}

	// then create its intent
	intent := &intents.Intent{
		DB:       db,
		C:        collection,
		BSONPath: fullpath,
		Size:     file.Size(),
	}

	// finally, check if it has a .metadata.json file in its folder
	log.Logf(log.DebugLow, "scanning directory %v for metadata file", filepath.Dir(fullpath))
	entries, err := ioutil.ReadDir(filepath.Dir(fullpath))
	if err != nil {
		// try and carry on if we can
		log.Logf(log.Info, "error attempting to locate metadata for file: %v", err)
		log.Log(log.Info, "restoring collection without metadata")
		restore.manager.Put(intent)
		return nil
	}
	metadataName := baseName + ".metadata.json"
	for _, entry := range entries {
		if entry.Name() == metadataName {
			metadataPath := filepath.Join(filepath.Dir(fullpath), metadataName)
			log.Logf(log.Info, "found metadata for collection at %v", metadataPath)
			intent.MetadataPath = metadataPath
			break
		}
	}

	if intent.MetadataPath == "" {
		log.Log(log.Info, "restoring collection without metadata")
	}

	restore.manager.Put(intent)

	return nil
}

// readDirWithSymlinks acts like ioutil.ReadDir, except symlinks are treated
// as directories instead of regular files
func readDirWithSymlinks(fullpath string) ([]os.FileInfo, error) {
	entries, err := ioutil.ReadDir(fullpath)
	if err != nil {
		return nil, err
	}
	for i, entry := range entries {
		if entry.Mode()&os.ModeSymlink != 0 {
			// update to a symlinked directory if the symlink flag is set
			entries[i] = symlinkedDir{entry, fullpath}
		}
	}
	return entries, nil

}

// SymlinkedDir wraps a FileInfo interface to treat symlinked directories as real
type symlinkedDir struct {
	os.FileInfo
	fullpath string
}

// return true if the OS can get at the directory
func (dir symlinkedDir) IsDir() bool {
	_, err := ioutil.ReadDir(filepath.Join(dir.fullpath, dir.Name()))
	return err == nil
}

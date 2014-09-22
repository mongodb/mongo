package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	"io/ioutil"
	"os"
	"path/filepath"
	"strings"
)

// Filename helpers
// TODO: MOVE THIS STUFF TO ITS OWN FILE

type FileType uint

const (
	UnknownFileType FileType = iota
	BSONFileType
	MetadataFileType
	//TODO users/roles?
)

// ExtractInfoFromFilename pulls the base collection name and
// type of file from a .bson/.metadata.json file
func ExtractInfoFromFilename(filename string) (string, FileType) {
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
	log.Logf(3, "using %v as dump root directory", fullpath)
	entries, err := ioutil.ReadDir(fullpath)
	if err != nil {
		return fmt.Errorf("error reading root dump folder: %v", err)
	}
	for _, entry := range entries {
		if entry.IsDir() {
			//TODO name validation
			err = restore.CreateIntentsForDB(entry.Name(),
				filepath.Join(fullpath, entry.Name()))
			if err != nil {
				return err
			}
		} else {
			if entry.Name() == "oplog.bson" {
				//TODO handle oplog
			} else {
				log.Logf(0, `don't know what to do with file "%v", skipping...`,
					filepath.Join(fullpath, entry.Name()))
			}
		}
	}
	return nil
}

// CreateIntentsForDB drills down into a db folder, creating intents
// for all of the collection dump files it encounters
func (restore *MongoRestore) CreateIntentsForDB(db, fullpath string) error {
	//TODO check that it's really a directory
	log.Logf(3, "reading collections for database %v in %v", db, fullpath)
	entries, err := ioutil.ReadDir(fullpath)
	if err != nil {
		return fmt.Errorf("error reading db folder %v: %v", db, err)
	}
	//TODO check if we still want to even deal with this
	usesMetadataFiles := hasMetadataFiles(entries)
	for _, entry := range entries {
		if entry.IsDir() {
			log.Logf(0, `don't know what to do with subdirectory "%v", skipping...`,
				filepath.Join(fullpath, entry.Name()))
		} else {
			//TODO handle user/roles?
			collection, fileType := ExtractInfoFromFilename(entry.Name())
			switch fileType {
			case BSONFileType:
				// skip resorting the indexes collection if we are using metadata
				// files to store index information, to eliminate redundancy
				if collection == "system.indexes" && usesMetadataFiles {
					log.Logf(2,
						"not restoring system.indexes collection because database %v "+
							"has .metadata.json files", db)
					continue
				}
				intent := &Intent{
					DB:       db,
					C:        collection,
					BSONPath: filepath.Join(fullpath, entry.Name()),
				}
				log.Logf(1, "found collection %v bson to restore", intent.Key())
				restore.manager.Put(intent)
			case MetadataFileType:
				usesMetadataFiles = true
				intent := &Intent{
					DB:           db,
					C:            collection,
					MetadataPath: filepath.Join(fullpath, entry.Name()),
				}
				log.Logf(1, "found collection %v metadata to restore", intent.Key())
				restore.manager.Put(intent)
			default:
				log.Logf(0, `don't know what to do with file "%v", skipping...`,
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

// TODO
func (restore *MongoRestore) CreateIntentForCollection(
	db, collection, fullpath string) error {
	log.Logf(2, "reading collection %v for database %v from %v",
		collection, db, fullpath)

	// first make sure the bson file exists and is valid
	file, err := os.Lstat(fullpath)
	if err != nil {
		return err
	}
	if file.IsDir() {
		return fmt.Errorf("file %v is a directory, not a bson file", fullpath)
	}

	baseName, fileType := ExtractInfoFromFilename(file.Name())
	if fileType != BSONFileType {
		return fmt.Errorf("file %v do, not a bson filees not have .bson extension", fullpath)
	}

	// then create its intent
	intent := &Intent{
		DB:       db,
		C:        collection,
		BSONPath: fullpath,
	}

	// finally, check if it has a .metadata.json file in its folder
	log.Logf(2, "scanning directory %v for metadata file", filepath.Dir(fullpath))
	entries, err := ioutil.ReadDir(filepath.Dir(fullpath))
	if err != nil {
		// try and carry on if we can
		log.Logf(0, "error attempting to locate metadata for file: %v", err)
		log.Log(0, "restoring collection without metadata")
		restore.manager.Put(intent)
		return nil
	}
	metadataName := baseName + ".metadata.json"
	for _, entry := range entries {
		if entry.Name() == metadataName {
			metadataPath := filepath.Join(filepath.Dir(fullpath), metadataName)
			log.Logf(0, "found metadata for collection at %v", metadataPath)
			intent.MetadataPath = metadataPath
			break
		}
	}

	if intent.MetadataPath == "" {
		log.Log(1, "restoring collection without metadata")
	}

	restore.manager.Put(intent)

	return nil
}

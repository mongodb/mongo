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

// FileType describes the various types of restore documents.
type FileType uint

// File types constants used by mongorestore.
const (
	UnknownFileType FileType = iota
	BSONFileType
	MetadataFileType
)

// GetInfoFromFilename pulls the base collection name and FileType from a given file.
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
		baseName := strings.TrimSuffix(baseFileName, ".bin")
		return baseName, BSONFileType
	case strings.HasSuffix(baseFileName, ".bson"):
		baseName := strings.TrimSuffix(baseFileName, ".bson")
		return baseName, BSONFileType
	default:
		return "", UnknownFileType
	}
}

// CreateAllIntents drills down into a dump folder, creating intents for all of
// the databases and collections it finds.
func (restore *MongoRestore) CreateAllIntents(dumpDir string) error {
	log.Logf(log.DebugHigh, "using %v as dump root directory", dumpDir)
	foundOplog := false
	entries, err := readDirWithSymlinks(dumpDir)
	if err != nil {
		return fmt.Errorf("error reading root dump folder: %v", err)
	}
	for _, entry := range entries {
		if entry.IsDir() {
			if err = util.ValidateDBName(entry.Name()); err != nil {
				return fmt.Errorf("invalid database name '%v': %v", entry.Name(), err)
			}
			err = restore.CreateIntentsForDB(entry.Name(), filepath.Join(dumpDir, entry.Name()))
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
					C:        "oplog",
					BSONPath: filepath.Join(dumpDir, entry.Name()),
					Size:     entry.Size(),
				})
			} else {
				log.Logf(log.Always, `don't know what to do with file "%v", skipping...`,
					filepath.Join(dumpDir, entry.Name()))
			}
		}
	}
	if restore.InputOptions.OplogReplay && !foundOplog {
		return fmt.Errorf("no %v/oplog.bson file to replay; make sure you run mongodump with --oplog", dumpDir)
	}
	return nil
}

// CreateIntentsForDB drills down into the dir folder, creating intents
// for all of the collection dump files it finds for the db database.
func (restore *MongoRestore) CreateIntentsForDB(db, dir string) error {

	log.Logf(log.DebugHigh, "reading collections for database %v in %v", db, dir)
	entries, err := ioutil.ReadDir(dir)
	if err != nil {
		return fmt.Errorf("error reading db folder %v: %v", db, err)
	}
	usesMetadataFiles := hasMetadataFiles(entries)
	for _, entry := range entries {
		if entry.IsDir() {
			log.Logf(log.Always, `don't know what to do with subdirectory "%v", skipping...`,
				filepath.Join(dir, entry.Name()))
		} else {
			collection, fileType := GetInfoFromFilename(entry.Name())
			switch fileType {
			case BSONFileType:
				// Dumps of a single database (i.e. with the -d flag) may contain special
				// db-specific collections that start with a "$" (for example, $admin.system.users
				// holds the users for a database that was dumped with --dumpDbUsersAndRoles enabled).
				// If these special files manage to be included in a dump directory during a full
				// (multi-db) restore, we should ignore them.
				if restore.ToolOptions.DB == "" && strings.HasPrefix(collection, "$") {
					log.Logf(log.DebugLow,
						"not restoring special collection %v.%v", db, collection)
					continue
				}
				// TOOLS-717: disallow restoring to the system.profile collection.
				// Server versions >= 3.0.3 disallow user inserts to system.profile so
				// it would likely fail anyway.
				if collection == "system.profile" {
					log.Logf(log.DebugLow, "skipping restore of system.profile collection", db)
					continue
				}
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
					BSONPath: filepath.Join(dir, entry.Name()),
				}
				log.Logf(log.Info, "found collection %v bson to restore", intent.Namespace())
				restore.manager.Put(intent)
			case MetadataFileType:
				usesMetadataFiles = true
				intent := &intents.Intent{
					DB:           db,
					C:            collection,
					MetadataPath: filepath.Join(dir, entry.Name()),
				}
				log.Logf(log.Info, "found collection %v metadata to restore", intent.Namespace())
				restore.manager.Put(intent)
			default:
				log.Logf(log.Always, `don't know what to do with file "%v", skipping...`,
					filepath.Join(dir, entry.Name()))
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

// CreateIntentForCollection builds an intent for the given database and collection name
// along with a path to a .bson collection file. It searches the file's parent directory
// for a matching metadata file.
//
// This method is not called by CreateIntentsForDB,
// it is only used in the case where --db and --collection flags are set.
func (restore *MongoRestore) CreateIntentForCollection(db, collection, fullpath string) error {

	log.Logf(log.DebugLow, "reading collection %v for database %v from %v",
		collection, db, fullpath)

	// avoid actual file handling if we are using stdin
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

// small helper that checks if the file pointed to is not a directory.
func isBSON(path string) bool {
	file, err := os.Lstat(path)
	if err != nil {
		// swallow error -- it will come up again in a place
		// that provides more helpful context to the user
		return false
	}
	return !file.IsDir()
}

// handleBSONInsteadOfDirectory updates -d and -c settings based on
// the path to the BSON file passed to mongorestore. This is only
// applicable if the target path points to a .bson file.
//
// As an example, when the user passes 'dump/mydb/col.bson', this method
// will infer that 'mydb' is the database and 'col' is the collection name.
func (restore *MongoRestore) handleBSONInsteadOfDirectory(path string) error {
	// we know we have been given a non-directory, so we should handle it
	// like a bson file and infer as much as we can
	if restore.ToolOptions.Collection == "" {
		// if the user did not set -c, use the file name for the collection
		newCollectionName, fileType := GetInfoFromFilename(path)
		if fileType != BSONFileType {
			return fmt.Errorf("file %v does not have .bson extension", path)
		}
		restore.ToolOptions.Collection = newCollectionName
		log.Logf(log.DebugLow, "inferred collection '%v' from file", restore.ToolOptions.Collection)
	}
	if restore.ToolOptions.DB == "" {
		// if the user did not set -d, use the directory containing the target
		// file as the db name (as it would be in a dump directory). If
		// we cannot determine the directory name, use "test"
		dirForFile := filepath.Base(filepath.Dir(path))
		if dirForFile == "." || dirForFile == ".." {
			dirForFile = "test"
		}
		restore.ToolOptions.DB = dirForFile
		log.Logf(log.DebugLow, "inferred db '%v' from the file's directory", restore.ToolOptions.DB)
	}
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

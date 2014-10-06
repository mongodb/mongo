package mongofiles

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongofiles/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"regexp"
	"time"
)

const (
	// list of possible commands for mongofiles
	List   = "list"
	Search = "search"
	Put    = "put"
	Get    = "get"
	Delete = "delete"

	// 'system.indexes' collection
	SystemIndexes = "system.indexes"

	// prefix for grid fs collections
	GridFSPrefix = "fs"

	// files collection
	GridFSFiles = GridFSPrefix + ".files"

	// chunks collection
	GridFSChunks = GridFSPrefix + ".chunks"

	// default chunk size for a GridFS file -- 255KB
	DefaultChunkSize = 255 * 1024
)

type MongoFiles struct {
	// generic mongo tool options
	ToolOptions *commonOpts.ToolOptions

	// mongofiles-specific storage options
	StorageOptions *options.StorageOptions

	// for connecting to the db
	cmdRunner db.CommandRunner

	// command to run
	Command string
	// filename in GridFS
	FileName string
}

// represents a GridFS file
type GFSFile struct {
	Id          bson.ObjectId `bson:"_id"`
	ChunkSize   int           `bson:"chunkSize"`
	FileName    string        `bson:"filename"`
	Length      int64         `bson:"length"`
	Md5         string        `bson:"md5"`
	UploadDate  time.Time     `bson:"uploadDate"`
	ContentType string        `bson:"contentType,omitempty"`
}

// represents a GridFS chunk
type GFSChunk struct {
	FilesId  bson.ObjectId `bson:"files_id"`
	ChunkNum int           `bson:"n"`
	Data     []byte        `bson:"data"`
}

// for storing md5 hash for GridFS file
type FileMD5 struct {
	Ok  bool   `bson:"ok"`
	Md5 string `bson:"md5,omitempty"`
}

// for storing result from a 'createIndexes' command
type CreateIndexesResult struct {
	Ok     bool   `bson:"ok"`
	Code   int    `bson:"code"`
	ErrMsg string `bson:"errmsg"`
}

func ValidateCommand(args []string) (string, error) {
	// make sure a command is specified and that we don't have
	// too many arguments
	if len(args) == 0 {
		return "", fmt.Errorf("you must specify a command")
	} else if len(args) > 2 {
		return "", fmt.Errorf("too many positional arguments")
	}

	var fileName string
	switch args[0] {
	case List:
		if len(args) == 1 {
			fileName = ""
		} else {
			fileName = args[1]
		}
	case Search, Put, Get, Delete:
		// also make sure the supporting argument isn't literally an empty string
		// for example, mongofiles get ""
		if len(args) == 1 || args[1] == "" {
			return "", fmt.Errorf("'%v' requires a non-empty supporting argument", args[0])
		}
		fileName = args[1]
	default:
		return "", fmt.Errorf("'%v' is not a valid command", args[0])
	}

	return fileName, nil
}

// query GridFS for files and display the results
func (self *MongoFiles) findAndDisplay(query bson.M) (string, error) {
	display := ""

	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, GridFSFiles,
		0, 0, query, []string{}, 0)
	if err != nil {
		return "", fmt.Errorf("error finding GridFS files : %v", err)
	}
	defer docSource.Close()

	var fileResult GFSFile
	for docSource.Next(&fileResult) {
		display += fmt.Sprintf("%s\t%d\n", fileResult.FileName, fileResult.Length)
	}
	if err := docSource.Err(); err != nil {
		return "", fmt.Errorf("error retrieving list of GridFS files: %v", err)
	}

	return display, nil
}

// initialize cmdRunner to use either shim or session provider
func (self *MongoFiles) Init() error {
	if self.ToolOptions.Namespace.DB == "" {
		self.ToolOptions.Namespace.DB = "test"
	} else {
		if err := util.ValidateDBName(self.ToolOptions.Namespace.DB); err != nil {
			return err
		}
	}

	// initialize logger
	log.InitToolLogger(self.ToolOptions.Verbosity)

	log.Logf(2, "initializing mongofiles tool for db: '%v'", self.ToolOptions.Namespace.DB)

	if self.ToolOptions.Namespace.DBPath != "" {
		shim, err := db.NewShim(
			self.ToolOptions.Namespace.DBPath,
			self.ToolOptions.DirectoryPerDB,
			self.ToolOptions.Journal,
		)
		if err != nil {
			return err
		}
		self.cmdRunner = shim

		log.Log(2, "using mongoshim to access database datafiles")
		return nil
	}

	self.cmdRunner = db.NewSessionProvider(*self.ToolOptions)
	return nil
}

// Return local file (set by --local optional flag) name (or default to self.FileName)
func (self *MongoFiles) getLocalFileName() string {
	localFileName := self.StorageOptions.LocalFileName
	if localFileName == "" {
		localFileName = self.FileName
	}
	return localFileName
}

// handle logic for 'get' command
func (self *MongoFiles) handleGet() (string, error) {
	var fileResult GFSFile
	err := self.cmdRunner.FindOne(self.ToolOptions.Namespace.DB, GridFSFiles, 0,
		bson.M{"filename": self.FileName}, []string{}, &fileResult, 0)
	if err != nil {
		return "", fmt.Errorf("error retrieving '%v' from GridFS: %v", self.FileName, err)
	}

	// create local file
	localFileName := self.getLocalFileName()
	localFile, err := os.Create(localFileName)
	if err != nil {
		return "", fmt.Errorf("error creating '%v', '%v'", localFileName, err)
	}
	defer localFile.Close()
	log.Logf(2, "created local file '%v'", localFileName)

	// read chunks for file
	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, GridFSChunks, 0, 0, bson.M{"files_id": fileResult.Id}, []string{"n"}, 0)
	if err != nil {
		return "", fmt.Errorf("error retrieving chunks for '%v': %v", self.FileName, err)
	}
	defer docSource.Close()
	log.Logf(2, "retrieving data chunks for GridFS file '%v'...", self.FileName)

	var chunkResult GFSChunk
	var dataBytes []byte
	chunkNum := 1
	for docSource.Next(&chunkResult) {
		dataBytes = chunkResult.Data
		_, err = localFile.Write(dataBytes)
		if err != nil {
			return "", fmt.Errorf("error while writing to file '%v' : %v", localFileName, err)
		}

		log.Logf(2, "retrieving chunk %d for '%v'", chunkNum, self.FileName)
		chunkNum++
	}
	if err = docSource.Err(); err != nil {
		return "", fmt.Errorf("error reading data for '%v' : %v", self.FileName, err)
	}

	log.Logf(2, "done writing data chunks for GridFS file '%v' to local file '%v'", self.FileName, localFileName)

	return fmt.Sprintf("Finished writing to: %s\n", localFileName), nil
}

// handle logic for 'put' command
func (self *MongoFiles) handlePut() (string, error) {
	localFileName := self.getLocalFileName()

	var output string

	// check if --replace flag turned on
	if self.StorageOptions.Replace {
		err := self.removeGridFSFile(self.FileName)
		if err != nil {
			return "", fmt.Errorf("error while trying to remove '%v' from GridFS: %v\n", self.FileName, err)
		}
		output = fmt.Sprintf("Removed all instances of '%v' from GridFS\n", self.FileName)
	}

	localFile, err := os.Open(localFileName)
	if err != nil {
		return "", fmt.Errorf("error while opening local file '%v' : %v\n", localFileName, err)
	}
	defer localFile.Close()
	log.Logf(2, "creating GridFS file '%v' from local file '%v'", self.FileName, localFileName)

	err = self.createGridFSFile(self.FileName, self.StorageOptions.ContentType, localFile)
	if err != nil {
		return "", fmt.Errorf("error while creating '%v' in GridFS: %v\n", self.FileName, err)
	}

	output += fmt.Sprintf("added file: %v\n", self.FileName)
	return output, nil
}

// remove file from GridFS
func (self *MongoFiles) removeGridFSFile(fileName string) error {
	errorStr := fmt.Sprintf("error removing '%v' from GridFS : %%v", fileName)

	// find all files with that name
	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, GridFSFiles, 0, 0, bson.M{"filename": fileName}, []string{}, 0)
	if err != nil {
		return fmt.Errorf(errorStr, err)
	}
	defer docSource.Close()

	var fileResult GFSFile
	for docSource.Next(&fileResult) {
		// remove file from GridFSFiles collection
		err = self.cmdRunner.Remove(self.ToolOptions.Namespace.DB, GridFSFiles, bson.M{"_id": fileResult.Id})
		if err != nil {
			return fmt.Errorf(errorStr, err)
		}
		log.Logf(2, "removed file '%v': %+v", fileName, fileResult)

		// remove file chunks from GridFSChunks collection
		err = self.cmdRunner.Remove(self.ToolOptions.Namespace.DB, GridFSChunks, bson.M{"files_id": fileResult.Id})
		log.Logf(2, "removed all chunks for '%v': %+v", fileName, fileResult)
	}
	if err := docSource.Err(); err != nil {
		return fmt.Errorf(errorStr, err)
	}

	return nil
}

// creates an index
func (self *MongoFiles) createIndex(collection string, indexDoc bsonutil.MarshalD, indexName string, isUnique bool) error {
	log.Logf(2, "creating index with key '%#v' for the %v collection...", indexDoc, collection)

	var createIndexesResult CreateIndexesResult
	createIndexCommand := bsonutil.MarshalD{
		{"createIndexes", collection}, {"indexes", []interface{}{
			bson.M{
				"key":    indexDoc,
				"name":   indexName,
				"unique": isUnique,
			},
		}}}
	err := self.cmdRunner.Run(createIndexCommand, &createIndexesResult, self.ToolOptions.Namespace.DB)
	if err != nil {
		return fmt.Errorf("error creating indexes on collection '%v': %v", collection, err)
	}
	if !createIndexesResult.Ok {
		return fmt.Errorf("indexes not created on collection '%v': %+v", collection, createIndexesResult)
	}
	log.Logf(2, "index creation result: %+v", createIndexesResult)

	return nil
}

// ensure index
func (self *MongoFiles) ensureIndex(collection string, indexDoc bsonutil.MarshalD, indexName string, isUnique bool) error {
	// using FindDocs instead of FindOne because of FindOne's error semantics
	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, SystemIndexes, 0, 0,
		bsonutil.MarshalD{{"key", indexDoc}}, []string{}, 0)
	if err != nil {
		return fmt.Errorf("error checking for indexes on the '%v' collection: %v", collection, err)
	}
	log.Logf(2, "checking for indexes on the %v collection", collection)

	var result bson.M
	indexExists := docSource.Next(&result)

	if err = docSource.Err(); err != nil {
		return fmt.Errorf("error retrieving indexes on the '%v' collection: %v", collection, err)
	}

	// must close before creating index because if using shim we can only have one shim open at a time
	err = docSource.Close()
	if err != nil {
		return fmt.Errorf("error closing shim: %v", err)
	}

	if !indexExists {
		// if an index doesn't exist, create one
		err = self.createIndex(collection, indexDoc, indexName, isUnique)
		if err != nil {
			return fmt.Errorf("error creating an index: %v", err)
		}
	}

	return nil
}

// creates a GridFS file and copies over data from a the local file 'localFSFile'
func (self *MongoFiles) createGridFSFile(gridFSFileName, contentType string, localFSFile *os.File) error {

	// ensure indexes exist on "filename" field in GridFSFiles and "files_id","n" in GridFSChunks
	err := self.ensureIndex(GridFSFiles, bsonutil.MarshalD{{"filename", 1}}, "filename_1", false)
	if err != nil {
		return fmt.Errorf("error creating indexes on the fs.files collection: %v", err)
	}
	err = self.ensureIndex(GridFSChunks, bsonutil.MarshalD{{"files_id", 1}, {"n", 1}}, "files_id_n_1", true)
	if err != nil {
		return fmt.Errorf("error creating indexes on the fs.chunks collection: %v", err)
	}

	// construct file info
	// add in other attributes (especially the time-critical 'uploadDate') immediately
	// before insertion into the DB
	newFile := GFSFile{
		Id:        bson.NewObjectId(),
		ChunkSize: DefaultChunkSize,
		FileName:  gridFSFileName,
	}

	// open GridFSChunks DocSink to write to
	chunksSink, err := self.cmdRunner.OpenInsertStream(self.ToolOptions.Namespace.DB, GridFSChunks, &mgo.Safe{})
	if err != nil {
		return fmt.Errorf("error while trying to open stream for writing chunks: %v", err)
	}
	log.Logf(2, "opened fs.chunks sink")

	// construct chunks for this file
	chunkBytes := make([]byte, DefaultChunkSize)
	length := int64(0)
	chunkNum := 0

	newChunk := GFSChunk{
		FilesId: newFile.Id,
	}

	var nRead int
	for {
		nRead, err = localFSFile.Read(chunkBytes)
		if err != nil && err != io.EOF {
			chunksSink.Close()
			return fmt.Errorf("error while reading in file '%v' : %v",
				localFSFile.Name(),
				err)
		}

		if nRead == 0 {
			break
		}

		length += int64(nRead)

		newChunk.ChunkNum = chunkNum
		chunkNum++
		newChunk.Data = chunkBytes[:nRead]
		err = chunksSink.WriteDoc(newChunk)
		if err != nil {
			chunksSink.Close()
			return fmt.Errorf("error while trying to write chunks to the database: %v", err)
		}
	}
	err = chunksSink.Close()
	if err != nil {
		return fmt.Errorf("error while trying to close write stream for chunks: %v", err)
	}
	log.Logf(2, "closed fs.chunks sink without error")

	// set length, md5, uploadDate, and (if applicable) contentType

	// length
	newFile.Length = length
	log.Logf(2, "length of file '%v': %d bytes", self.FileName, length)

	// md5
	var md5Res FileMD5
	command := bsonutil.MarshalD{{"filemd5", newFile.Id}, {"root", GridFSPrefix}}
	err = self.cmdRunner.Run(command, &md5Res, self.ToolOptions.Namespace.DB)
	if err != nil {
		return fmt.Errorf("error while trying to compute md5: %v", err)
	}
	if !md5Res.Ok {
		return fmt.Errorf("invalid command to retrieve md5: %v", command)
	}
	newFile.Md5 = md5Res.Md5
	log.Logf(2, "md5 for file '%v': %v", self.FileName, md5Res.Md5)

	// upload date
	newFile.UploadDate = time.Now()
	log.Logf(2, "upload date for file '%v': %v", self.FileName, newFile.UploadDate)

	// content type
	if contentType != "" {
		newFile.ContentType = contentType
		log.Logf(2, "setting content/MIME type for file '%v' to '%v'", self.FileName, contentType)
	}

	// open GridFSFiles DocSink to write to
	filesSink, err := self.cmdRunner.OpenInsertStream(self.ToolOptions.Namespace.DB, GridFSFiles, &mgo.Safe{})
	if err != nil {
		return fmt.Errorf("error while trying to open stream for inserting file information into %s: %v", GridFSFiles, err)
	}
	log.Log(2, "fs.files sink opened")

	err = filesSink.WriteDoc(newFile)
	if err != nil {
		return fmt.Errorf("error while trying to write file information into %s: %v", GridFSFiles, err)
	}
	log.Log(2, "finished writing to fs.files sink")

	err = filesSink.Close()
	log.Log(2, "closing fs.files sink")
	if err != nil {
		return fmt.Errorf("error while trying to close write stream for files: %v", err)
	}

	return nil
}

// Run the mongofiles utility
func (self *MongoFiles) Run(displayConnUrl bool) (string, error) {
	if displayConnUrl {
		connHost := self.ToolOptions.Host
		if connHost == "" {
			connHost = util.DefaultHost
		}
		connPort := self.ToolOptions.Port
		if connPort == "" {
			connPort = util.DefaultPort
		}
		
		fmt.Printf("connected to: %v\n", fmt.Sprintf("%s:%s", connHost, connPort))
	}

	var output string
	var err error

	log.Logf(1, "handling mongofiles '%v' command...", self.Command)

	switch self.Command {

	case List:

		query := bson.M{}
		if self.FileName != "" {
			regex := bson.M{"$regex": "^" + regexp.QuoteMeta(self.FileName)}
			query = bson.M{"filename": regex}
		}

		output, err = self.findAndDisplay(query)
		if err != nil {
			return "", err
		}

	case Search:

		regex := bson.M{"$regex": regexp.QuoteMeta(self.FileName)}
		query := bson.M{"filename": regex}

		output, err = self.findAndDisplay(query)
		if err != nil {
			return "", err
		}

	case Get:

		output, err = self.handleGet()
		if err != nil {
			return "", err
		}

	case Put:

		output, err = self.handlePut()
		if err != nil {
			return "", err
		}

	case Delete:

		err = self.removeGridFSFile(self.FileName)
		if err != nil {
			return "", fmt.Errorf("error while removing '%v' from GridFS: %v\n", self.FileName, err)
		}
		output = fmt.Sprintf("successfully deleted all instances of '%v' from GridFS\n", self.FileName)

	}

	return output, nil
}

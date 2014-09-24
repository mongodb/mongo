package mongofiles

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongofiles/options"
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
	ToolOptions *commonopts.ToolOptions

	// mongofiles-specific storage options
	StorageOptions *options.StorageOptions

	// for connecting to the db
	cmdRunner db.CommandRunner

	// command to run
	Command string
	// filename in GridFS
	FileName string
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
		// for example, ./mongofiles get ""
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

	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, GridFSFiles, 0, 0, query, []string{}, 0)
	if err != nil {
		return "", fmt.Errorf("error finding GridFS files : %v", err)
	}
	defer docSource.Close()

	var result bson.M
	for docSource.Next(&result) {
		display += fmt.Sprintf("%s\t%d\n", result["filename"], result["length"])
	}
	if err := docSource.Err(); err != nil {
		return "", fmt.Errorf("error retrieving list of GridFS files: %v", err)
	}

	return display, nil
}

// initialize cmdRunner to use either shim or session provider
func (self *MongoFiles) Init() error {
	if self.ToolOptions.Namespace.DBPath != "" {
		shim, err := db.NewShim(self.ToolOptions.Namespace.DBPath,
			self.ToolOptions.DirectoryPerDB,
			self.ToolOptions.Journal)
		if err != nil {
			return err
		}
		self.cmdRunner = shim
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

// Return ObjectID in extended JSON format
func getExtendedJSONOID(oid interface{}) (bson.M, bool) {
	oidCast, ok := oid.(bson.ObjectId)
	if !ok {
		return nil, false
	}
	return bson.M{"$oid": oidCast.Hex()}, true
}

// handle logic for 'get' command
func (self *MongoFiles) handleGet() (string, error) {
	var fileResult bson.M
	err := self.cmdRunner.FindOne(self.ToolOptions.Namespace.DB, GridFSFiles, 0, bson.M{"filename": self.FileName}, []string{}, &fileResult, 0)
	if err != nil {
		return "", fmt.Errorf("error retrieving '%v' from GridFS: %v", self.FileName, err)
	}

	// create local file
	localFileName := self.getLocalFileName()
	localFile, err := os.Create(localFileName)
	if err != nil {
		return "", fmt.Errorf("error creating '%v', '%v'", localFileName, err)
	}

	fileOID := fileResult["_id"]
	if self.ToolOptions.Namespace.DBPath != "" {
		// encode fileOID in extended JSON if using shim
		var ok bool
		if fileOID, ok = getExtendedJSONOID(fileOID); !ok {
			return "", fmt.Errorf("invalid ObjectID '%v' for file '%v'", fileOID, self.FileName)
		}
	}

	// read chunks for file
	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, GridFSChunks, 0, 0, bson.M{"files_id": fileOID}, []string{}, 0)
	if err != nil {
		return "", fmt.Errorf("error retrieving chunks for '%v': %v", self.FileName, err)
	}
	defer docSource.Close()

	var result bson.M
	var dataBytes []byte
	var ok bool
	for docSource.Next(&result) {
		if dataBytes, ok = result["data"].([]byte); !ok {
			return "", fmt.Errorf("error reading data for '%v'", self.FileName)
		}
		_, err = localFile.Write(dataBytes)
		if err != nil {
			return "", fmt.Errorf("error while writing to file '%v' : %v", localFileName, err)
		}
	}

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

	err = self.createGridFSFile(self.FileName, self.StorageOptions.ContentType, localFile)
	if err != nil {
		return "", fmt.Errorf("error while creating '%v' in GridFS: %v\n", self.FileName, err)
	}

	output += fmt.Sprintf("added file: %v\n", self.FileName)
	return output, nil
}

// remove file from GridFS
func (self *MongoFiles) removeGridFSFile(fileName string) error {
	errorStr := "error removing '%v' from GridFS : %v"
	// find all files with that name
	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, GridFSFiles, 0, 0, bson.M{"filename":fileName}, []string{}, 0)
	if err != nil {
		return fmt.Errorf(errorStr, fileName, err)
	}
	defer docSource.Close()

	var result bson.M
	for docSource.Next(&result) {
		fileOID := result["_id"]
		if self.ToolOptions.Namespace.DBPath != "" {
			// encode fileOID in extended JSON if using shim
			var ok bool
			if fileOID, ok = getExtendedJSONOID(fileOID); !ok {
				return fmt.Errorf("invalid ObjectID '%v' for file '%v'", fileOID, fileName)
			}
		}
		// remove file from GridFSFiles collection
		err = self.cmdRunner.RemoveAll(self.ToolOptions.Namespace.DB, GridFSFiles, bson.M{"_id":fileOID})
		if err != nil {
			return fmt.Errorf(errorStr, fileName, err)
		}
		// remove file from GridFSChunks collection
		err = self.cmdRunner.RemoveAll(self.ToolOptions.Namespace.DB, GridFSChunks, bson.M{"files_id":fileOID})
	}
	if err := docSource.Err(); err != nil {
		return fmt.Errorf(errorStr, fileName, err)
	}

	return nil
}

// creates a GridFS file and copies over data from a the local file 'localFSFile'
func (self *MongoFiles) createGridFSFile(gridFSFilename, contentType string, localFSFile *os.File) error {

	// construct file info
	// add in other attributes (especially the time-critical 'uploadDate') immediately
	// before insertion into the DB
	newFile := bson.M{
		"_id":       bson.NewObjectId(),
		"chunkSize": DefaultChunkSize,
		"filename":  gridFSFilename,
	}

	// open GridFSChunks DocSink to write to
	chunksSink, err := self.cmdRunner.OpenInsertStream(self.ToolOptions.Namespace.DB, GridFSChunks)
	if err != nil {
		return err
	}

	// construct chunks for this file
	chunkBytes := make([]byte, DefaultChunkSize)
	length := int64(0)
	chunkNum := 0
	newChunk := bson.M{
		"files_id": newFile["_id"],
		"n":        chunkNum,
	}

	var nRead int
	for {
		nRead, err = localFSFile.Read(chunkBytes)
		if err != nil && err != io.EOF {
			return fmt.Errorf("error while reading in file '%v' : %v",
				localFSFile.Name(),
				err)
		}

		if nRead == 0 {
			break
		}

		length += int64(nRead)
		newChunk["data"] = chunkBytes[:nRead]
		err = chunksSink.WriteDoc(newChunk)
		if err != nil {
			return err
		}
		newChunk["n"] = chunkNum
		chunkNum++
	}
	chunksSink.Close()
	
	// set length, md5, uploadDate, and (if applicable) contentType

	// length
	newFile["length"] = length

	// md5
	var md5Res bson.M
	err = self.cmdRunner.Run(bson.M{"filemd5": newFile["_id"], "root": GridFSPrefix}, &md5Res, self.ToolOptions.Namespace.DB)
	if err != nil {
		return err
	}
	newFile["md5"] = md5Res["md5"]

	// upload date
	newFile["uploadDate"] = time.Now()

	// content type
	if contentType != "" {
		newFile["contentType"] = contentType
	}

	// open GridFSFiles DocSink to write to
	filesSink, err := self.cmdRunner.OpenInsertStream(self.ToolOptions.Namespace.DB, GridFSFiles)
	if err != nil {
		return err
	}
	defer filesSink.Close()

	err = filesSink.WriteDoc(newFile)
	if err != nil {
		return err
	}

	return nil
}

// Run the mongofiles utility
func (self *MongoFiles) Run() (string, error) {
	// get connection url
	connUrl := self.ToolOptions.Host
	if self.ToolOptions.Port != "" {
		connUrl = fmt.Sprintf("%s:%s", connUrl, self.ToolOptions.Port)
	}
	fmt.Printf("connected to: %v\n", connUrl)

	var output string
	var err error

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

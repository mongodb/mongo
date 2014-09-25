package mongofiles

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
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

	// read chunks for file
	docSource, err := self.cmdRunner.FindDocs(self.ToolOptions.Namespace.DB, GridFSChunks, 0, 0, bson.M{"files_id": fileResult.Id}, []string{"n"}, 0)
	if err != nil {
		return "", fmt.Errorf("error retrieving chunks for '%v': %v", self.FileName, err)
	}
	defer docSource.Close()

	var chunkResult GFSChunk
	var dataBytes []byte
	for docSource.Next(&chunkResult) {
		dataBytes = chunkResult.Data
		_, err = localFile.Write(dataBytes)
		if err != nil {
			return "", fmt.Errorf("error while writing to file '%v' : %v", localFileName, err)
		}
	}
	if err = docSource.Err(); err != nil {
		return "", fmt.Errorf("error reading data for '%v' : %v", self.FileName, err)
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
		// remove file from GridFSChunks collection
		err = self.cmdRunner.Remove(self.ToolOptions.Namespace.DB, GridFSChunks, bson.M{"files_id": fileResult.Id})
	}
	if err := docSource.Err(); err != nil {
		return fmt.Errorf(errorStr, err)
	}

	return nil
}

// creates a GridFS file and copies over data from a the local file 'localFSFile'
func (self *MongoFiles) createGridFSFile(gridFSFileName, contentType string, localFSFile *os.File) error {

	// construct file info
	// add in other attributes (especially the time-critical 'uploadDate') immediately
	// before insertion into the DB
	newFile := GFSFile{
		Id:        bson.NewObjectId(),
		ChunkSize: DefaultChunkSize,
		FileName:  gridFSFileName,
	}

	// open GridFSChunks DocSink to write to
	chunksSink, err := self.cmdRunner.OpenInsertStream(self.ToolOptions.Namespace.DB, GridFSChunks)
	if err != nil {
		return fmt.Errorf("error while trying to open stream for writing chunks: %v", err)
	}

	// construct chunks for this file
	chunkBytes := make([]byte, DefaultChunkSize)
	length := int64(0)
	chunkNum := 0

	newChunk := GFSChunk{
		FilesId:  newFile.Id,
		ChunkNum: chunkNum,
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

		newChunk.Data = chunkBytes[:nRead]
		err = chunksSink.WriteDoc(newChunk)
		if err != nil {
			chunksSink.Close()
			return fmt.Errorf("error while trying to write chunks to the database: %v", err)
		}

		newChunk.ChunkNum = chunkNum
		chunkNum++
	}
	err = chunksSink.Close()
	if err != nil {
		return fmt.Errorf("error while trying to close write stream for chunks: %v", err)
	}

	// set length, md5, uploadDate, and (if applicable) contentType

	// length
	newFile.Length = length

	// md5
	var md5Res FileMD5
	command := bsonutil.MarshalD{{"filemd5", newFile.Id}, {"root", GridFSPrefix}}
	err = self.cmdRunner.Run(command, &md5Res, "admin")
	if err != nil {
		return fmt.Errorf("error while trying to compute md5: %v", err)
	}
	if !md5Res.Ok {
		return fmt.Errorf("invalid command to retrieve md5: %v", command)
	}
	newFile.Md5 = md5Res.Md5

	// upload date
	newFile.UploadDate = time.Now()

	// content type
	if contentType != "" {
		newFile.ContentType = contentType
	}

	// open GridFSFiles DocSink to write to
	filesSink, err := self.cmdRunner.OpenInsertStream(self.ToolOptions.Namespace.DB, GridFSFiles)
	if err != nil {
		return fmt.Errorf("error while trying to open stream for inserting file information into %s: %v", GridFSFiles, err)
	}

	err = filesSink.WriteDoc(newFile)
	if err != nil {
		return fmt.Errorf("error while trying to write file information into %s: %v", GridFSFiles, err)
	}
	err = filesSink.Close()
	if err != nil {
		return fmt.Errorf("error while trying to close write stream for files: %v", err)
	}

	return nil
}

// Run the mongofiles utility
func (self *MongoFiles) Run(displayConnUrl bool) (string, error) {
	if displayConnUrl {
		// get connection url
		connUrl := self.ToolOptions.Host
		if self.ToolOptions.Port != "" {
			connUrl = fmt.Sprintf("%s:%s", connUrl, self.ToolOptions.Port)
		}
		fmt.Printf("connected to: %v\n", connUrl)
	}

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

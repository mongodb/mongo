// Package mongofiles provides an interface to GridFS collections in a MongoDB instance.
package mongofiles

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"regexp"
	"time"
)

// List of possible commands for mongofiles.
const (
	List   = "list"
	Search = "search"
	Put    = "put"
	Get    = "get"
	Delete = "delete"
)

// MongoFiles is a container for the user-specified options and
// internal state used for running mongofiles.
type MongoFiles struct {
	// generic mongo tool options
	ToolOptions *options.ToolOptions

	// mongofiles-specific storage options
	StorageOptions *StorageOptions

	// for connecting to the db
	SessionProvider *db.SessionProvider

	// command to run
	Command string

	// filename in GridFS
	FileName string
}

// GFSFile represents a GridFS file.
type GFSFile struct {
	Id          bson.ObjectId `bson:"_id"`
	ChunkSize   int           `bson:"chunkSize"`
	Name        string        `bson:"filename"`
	Length      int64         `bson:"length"`
	Md5         string        `bson:"md5"`
	UploadDate  time.Time     `bson:"uploadDate"`
	ContentType string        `bson:"contentType,omitempty"`
}

// ValidateCommand ensures the arguments supplied are valid.
func (mf *MongoFiles) ValidateCommand(args []string) error {
	// make sure a command is specified and that we don't have
	// too many arguments
	if len(args) == 0 {
		return fmt.Errorf("no command specified")
	} else if len(args) > 2 {
		return fmt.Errorf("too many positional arguments")
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
		// also make sure the supporting argument isn't literally an
		// empty string for example, mongofiles get ""
		if len(args) == 1 || args[1] == "" {
			return fmt.Errorf("'%v' argument missing", args[0])
		}
		fileName = args[1]
	default:
		return fmt.Errorf("'%v' is not a valid command", args[0])
	}

	if mf.StorageOptions.GridFSPrefix == "" {
		return fmt.Errorf("--prefix can not be blank")
	}

	// set the mongofiles command and file name
	mf.Command = args[0]
	mf.FileName = fileName
	return nil
}

// Query GridFS for files and display the results.
func (mf *MongoFiles) findAndDisplay(gfs *mgo.GridFS, query bson.M) (string, error) {
	display := ""

	cursor := gfs.Find(query).Iter()
	defer cursor.Close()

	var file GFSFile
	for cursor.Next(&file) {
		display += fmt.Sprintf("%s\t%d\n", file.Name, file.Length)
	}
	if err := cursor.Err(); err != nil {
		return "", fmt.Errorf("error retrieving list of GridFS files: %v", err)
	}

	return display, nil
}

// Return the local filename, as specified by the --local flag. Defaults to
// mf.FileName if not present.
func (mf *MongoFiles) getLocalFileName() string {
	localFileName := mf.StorageOptions.LocalFileName
	if localFileName == "" {
		localFileName = mf.FileName
	}
	return localFileName
}

// handle logic for 'get' command
func (mf *MongoFiles) handleGet(gfs *mgo.GridFS) (string, error) {
	gFile, err := gfs.Open(mf.FileName)
	if err != nil {
		return "", fmt.Errorf("error opening GridFS file '%s': %v", mf.FileName, err)
	}
	defer gFile.Close()

	localFileName := mf.getLocalFileName()
	localFile, err := os.Create(localFileName)
	if err != nil {
		return "", fmt.Errorf("error while opening local file '%v': %v\n", localFileName, err)
	}
	defer localFile.Close()
	log.Logf(log.DebugLow, "created local file '%v'", localFileName)

	_, err = io.Copy(localFile, gFile)
	if err != nil {
		return "", fmt.Errorf("error while writing data into local file '%v': %v\n", localFileName, err)
	}

	return fmt.Sprintf("Finished writing to: %s\n", localFileName), nil
}

// handle logic for 'put' command.
func (mf *MongoFiles) handlePut(gfs *mgo.GridFS) (string, error) {
	localFileName := mf.getLocalFileName()

	var output string

	// check if --replace flag turned on
	if mf.StorageOptions.Replace {
		err := gfs.Remove(mf.FileName)
		if err != nil {
			return "", err
		}
		output = fmt.Sprintf("removed all instances of '%v' from GridFS\n", mf.FileName)
	}

	localFile, err := os.Open(localFileName)
	if err != nil {
		return "", fmt.Errorf("error while opening local file '%v' : %v\n", localFileName, err)
	}
	defer localFile.Close()
	log.Logf(log.DebugLow, "creating GridFS file '%v' from local file '%v'", mf.FileName, localFileName)

	gFile, err := gfs.Create(mf.FileName)
	if err != nil {
		return "", fmt.Errorf("error while creating '%v' in GridFS: %v\n", mf.FileName, err)
	}
	defer gFile.Close()

	// set optional mime type
	if mf.StorageOptions.ContentType != "" {
		gFile.SetContentType(mf.StorageOptions.ContentType)
	}

	_, err = io.Copy(gFile, localFile)
	if err != nil {
		return "", fmt.Errorf("error while storing '%v' into GridFS: %v\n", localFileName, err)
	}

	output += fmt.Sprintf("added file: %v\n", gFile.Name())
	return output, nil
}

// Run the mongofiles utility. If displayHost is true, the connected host/port is
// displayed.
func (mf *MongoFiles) Run(displayHost bool) (string, error) {
	connUrl := mf.ToolOptions.Host
	if connUrl == "" {
		connUrl = util.DefaultHost
	}
	if mf.ToolOptions.Port != "" {
		connUrl = fmt.Sprintf("%s:%s", connUrl, mf.ToolOptions.Port)
	}

	// get session
	session, err := mf.SessionProvider.GetSession()
	if err != nil {
		return "", err
	}
	defer session.Close()

	// check if we are using a replica set and fall back to w=1 if we aren't (for <= 2.4)
	isRepl, err := mf.SessionProvider.IsReplicaSet()
	if err != nil {
		return "", fmt.Errorf("error determining if connected to replica set: %v", err)
	}

	safety, err := db.BuildWriteConcern(mf.StorageOptions.WriteConcern, isRepl)
	if err != nil {
		return "", fmt.Errorf("error parsing write concern: %v", err)
	}

	// configure the session with the appropriate write concern and ensure the
	// socket does not timeout
	session.SetSafe(safety)
	session.SetSocketTimeout(0)

	if displayConnUrl {
		log.Logf(log.Always, "connected to: %v\n", connUrl)
	}

	// first validate the namespaces we'll be using: <db>.<prefix>.files and <db>.<prefix>.chunks
	// it's ok to validate only <db>.<prefix>.chunks (the longer one)
	err = util.ValidateFullNamespace(fmt.Sprintf("%s.%s.chunks", mf.StorageOptions.DB,
		mf.StorageOptions.GridFSPrefix))

	if err != nil {
		return "", err
	}
	// get GridFS handle
	gfs := session.DB(mf.StorageOptions.DB).GridFS(mf.StorageOptions.GridFSPrefix)

	var output string

	log.Logf(log.Info, "handling mongofiles '%v' command...", mf.Command)

	switch mf.Command {

	case List:

		query := bson.M{}
		if mf.FileName != "" {
			regex := bson.M{"$regex": "^" + regexp.QuoteMeta(mf.FileName)}
			query = bson.M{"filename": regex}
		}

		output, err = mf.findAndDisplay(gfs, query)
		if err != nil {
			return "", err
		}

	case Search:

		regex := bson.M{"$regex": mf.FileName}
		query := bson.M{"filename": regex}

		output, err = mf.findAndDisplay(gfs, query)
		if err != nil {
			return "", err
		}

	case Get:

		output, err = mf.handleGet(gfs)
		if err != nil {
			return "", err
		}

	case Put:

		output, err = mf.handlePut(gfs)
		if err != nil {
			return "", err
		}

	case Delete:

		err = gfs.Remove(mf.FileName)
		if err != nil {
			return "", fmt.Errorf("error while removing '%v' from GridFS: %v\n", mf.FileName, err)
		}
		output = fmt.Sprintf("successfully deleted all instances of '%v' from GridFS\n", mf.FileName)

	}

	return output, nil
}

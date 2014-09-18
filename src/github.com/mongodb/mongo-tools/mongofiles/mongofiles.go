package mongofiles

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongofiles/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"regexp"
)

// list of possible commands
const (
	List   = "list"
	Search = "search"
	Put    = "put"
	Get    = "get"
	Delete = "delete"
)

type MongoFiles struct {
	// generic mongo tool options
	ToolOptions *commonopts.ToolOptions

	// mongofiles-specific storage options
	StorageOptions *options.StorageOptions

	// for connection to the db
	SessionProvider *db.SessionProvider

	// command to run
	Command string
	// filename in GridFS
	Filename string
}

func ValidateCommand(args []string) (string, error) {
	// make sure a command is specified and that we don't have
	// too many arguments
	if len(args) == 0 {
		return "", fmt.Errorf("You must specify a command")
	} else if len(args) > 2 {
		return "", fmt.Errorf("Too many positional arguments")
	}

	var filename string
	switch args[0] {
	case List:
		if len(args) == 1 {
			filename = ""
		} else {
			filename = args[1]
		}
	case Search, Put, Get, Delete:
		// also make sure the supporting argument isn't literally an empty string
		// for example, ./mongofiles get ""
		if len(args) == 1 || args[1] == "" {
			return "", fmt.Errorf("'%v' requires a non-empty supporting argument", args[0])
		}
		filename = args[1]
	default:
		return "", fmt.Errorf("'%v' is not a valid command", args[0])
	}

	return filename, nil
}

// query GridFS for files and display the results
func findAndDisplay(gfs *mgo.GridFS, query bson.M) (string, error) {
	display := ""

	cursor := gfs.Find(query).Iter()
	defer cursor.Close()

	var result bson.M
	for cursor.Next(&result) {
		display += fmt.Sprintf("%s\t%d\n", result["filename"], result["length"])
	}
	if err := cursor.Err(); err != nil {
		return "", err
	}

	return display, nil
}

// Return local file name (or default to self.Filename)
func (self *MongoFiles) getLocalFilename() string {
	localFilename := self.StorageOptions.Local
	if localFilename == "" {
		localFilename = self.Filename
	}
	return localFilename
}

// handle logic for 'get' command
func (self *MongoFiles) handleGet(gfs *mgo.GridFS) (string, error) {
	gFile, err := gfs.Open(self.Filename)
	if err != nil {
		return "", fmt.Errorf("Error: %v\n", err)
	}
	defer gFile.Close()

	localFilename := self.getLocalFilename()

	localFile, err := os.Create(localFilename)
	if err != nil {
		return "", fmt.Errorf("Error while opening local file '%v' : %v\n", localFilename, err)
	}
	defer localFile.Close()

	_, err = io.Copy(localFile, gFile)
	if err != nil {
		return "", fmt.Errorf("Error while writing data into local file '%v' : %v\n", localFilename, err)
	}

	return fmt.Sprintf("Finished writing to: %s\n", localFilename), nil
}

func (self *MongoFiles) handlePut(gfs *mgo.GridFS) (string, error) {
	var output string

	localFilename := self.getLocalFilename()

	// first, check if local file exists
	if _, err := os.Stat(localFilename); err != nil {
		if os.IsNotExist(err) {
			return "", fmt.Errorf("'%v' not found on local filesystem\n", localFilename)
		}
		return "", err
	}

	// check if --replace flag turned on
	if self.StorageOptions.Replace {
		err := gfs.Remove(self.Filename)
		if err != nil {
			return "", err
		}
		output = fmt.Sprintf("Removed all instances of '%v' from GridFS\n", self.Filename)
	}

	localFile, err := os.Open(localFilename)
	if err != nil {
		return "", fmt.Errorf("Error while opening local file '%v' : %v\n", localFilename, err)
	}
	defer localFile.Close()

	gFile, err := gfs.Create(self.Filename)
	if err != nil {
		return "", fmt.Errorf("Error while creating '%v' in GridFS: %v\n", self.Filename, err)
	}
	defer gFile.Close()

	// set optional mime type
	if self.StorageOptions.Type != "" {
		gFile.SetContentType(self.StorageOptions.Type)
	}

	_, err = io.Copy(gFile, localFile)
	if err != nil {
		return "", fmt.Errorf("Error while storing '%v' into GridFS: %v\n", localFilename, err)
	}

	output += fmt.Sprintf("added file: %v\n", gFile.Name())

	return output, nil
}

// Run the mongofiles utility
func (self *MongoFiles) Run() (string, error) {
	// get session
	session, err := self.SessionProvider.GetSession()
	if err != nil {
		return "", fmt.Errorf("Error connecting to db: %v", err)
	}
	defer session.Close()

	// get connection url
	connUrl := self.ToolOptions.Host
	if self.ToolOptions.Port != "" {
		connUrl = connUrl + ":" + self.ToolOptions.Port
	}
	fmt.Printf("connected to: %v\n", connUrl)

	// initialize GridFS
	gfs := session.DB(self.ToolOptions.Namespace.DB).GridFS("fs")

	var output string

	switch self.Command {

	case List:

		query := bson.M{}
		if self.Filename != "" {
			regex := bson.RegEx{"^" + regexp.QuoteMeta(self.Filename), ""}
			query = bson.M{"filename": regex}
		}

		output, err = findAndDisplay(gfs, query)
		if err != nil {
			return "", err
		}

	case Search:

		regex := bson.RegEx{regexp.QuoteMeta(self.Filename), ""}
		query := bson.M{"filename": regex}

		output, err = findAndDisplay(gfs, query)
		if err != nil {
			return "", err
		}

	case Get:

		output, err = self.handleGet(gfs)
		if err != nil {
			return "", err
		}

	case Put:

		output, err = self.handlePut(gfs)
		if err != nil {
			return "", err
		}

	case Delete:

		err = gfs.Remove(self.Filename)
		if err != nil {
			return "", fmt.Errorf("Error while removing '%v' from GridFS: %v\n", self.Filename, err)
		}
		output = fmt.Sprintf("successfully deleted all instances of '%v' from GridFS\n", self.Filename)

	}

	return output, nil
}

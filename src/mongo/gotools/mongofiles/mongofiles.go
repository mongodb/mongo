// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package mongofiles provides an interface to GridFS collections in a MongoDB instance.
package mongofiles

import (
	"fmt"
	"io"
	"os"
	"regexp"
	"time"

	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

// List of possible commands for mongofiles.
const (
	List     = "list"
	Search   = "search"
	Put      = "put"
	PutID    = "put_id"
	Get      = "get"
	GetID    = "get_id"
	Delete   = "delete"
	DeleteID = "delete_id"
)

// MongoFiles is a container for the user-specified options and
// internal state used for running mongofiles.
type MongoFiles struct {
	// generic mongo tool options
	ToolOptions *options.ToolOptions

	// mongofiles-specific storage options
	StorageOptions *StorageOptions

	// mongofiles-specific input options
	InputOptions *InputOptions

	// for connecting to the db
	SessionProvider *db.SessionProvider

	// command to run
	Command string

	// filename in GridFS
	FileName string

	//ID to put into GridFS
	Id string
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
	}

	switch args[0] {
	case List:
		if len(args) > 2 {
			return fmt.Errorf("too many positional arguments")
		}
		if len(args) == 1 {
			mf.FileName = ""
		} else {
			mf.FileName = args[1]
		}
	case Search, Put, Get, Delete:
		if len(args) > 2 {
			return fmt.Errorf("too many positional arguments")
		}
		// also make sure the supporting argument isn't literally an
		// empty string for example, mongofiles get ""
		if len(args) == 1 || args[1] == "" {
			return fmt.Errorf("'%v' argument missing", args[0])
		}
		mf.FileName = args[1]
	case GetID, DeleteID:
		if len(args) > 2 {
			return fmt.Errorf("too many positional arguments")
		}
		if len(args) == 1 || args[1] == "" {
			return fmt.Errorf("'%v' argument missing", args[0])
		}
		mf.Id = args[1]
	case PutID:
		if len(args) > 3 {
			return fmt.Errorf("too many positional arguments")
		}
		if len(args) < 3 || args[1] == "" || args[2] == "" {
			return fmt.Errorf("'%v' argument(s) missing", args[0])
		}
		mf.FileName = args[1]
		mf.Id = args[2]
	default:
		return fmt.Errorf("'%v' is not a valid command", args[0])
	}

	if mf.StorageOptions.GridFSPrefix == "" {
		return fmt.Errorf("--prefix can not be blank")
	}

	mf.Command = args[0]
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
// the GridFile's name if not present. If GridFile is nil, uses the filename
// given on the command line.
func (mf *MongoFiles) getLocalFileName(gridFile *mgo.GridFile) string {
	localFileName := mf.StorageOptions.LocalFileName
	if localFileName == "" {
		if gridFile != nil {
			localFileName = gridFile.Name()
		} else {
			localFileName = mf.FileName
		}
	}
	return localFileName
}

// handle logic for 'get' command
func (mf *MongoFiles) handleGet(gfs *mgo.GridFS) error {
	gFile, err := gfs.Open(mf.FileName)
	if err != nil {
		return fmt.Errorf("error opening GridFS file '%s': %v", mf.FileName, err)
	}
	defer gFile.Close()
	if err = mf.writeFile(gFile); err != nil {
		return err
	}
	log.Logvf(log.Always, fmt.Sprintf("finished writing to %s\n", mf.getLocalFileName(gFile)))
	return nil
}

// handle logic for 'get_id' command
func (mf *MongoFiles) handleGetID(gfs *mgo.GridFS) error {
	id, err := mf.parseID()
	if err != nil {
		return err
	}
	// with the parsed _id, grab the file and write it to disk
	gFile, err := gfs.OpenId(id)
	if err != nil {
		return fmt.Errorf("error opening GridFS file with _id %s: %v", mf.Id, err)
	}
	defer gFile.Close()
	if err = mf.writeFile(gFile); err != nil {
		return err
	}
	log.Logvf(log.Always, fmt.Sprintf("finished writing to: %s\n", mf.getLocalFileName(gFile)))
	return nil
}

// logic for deleting a file
func (mf *MongoFiles) handleDelete(gfs *mgo.GridFS) error {
	err := gfs.Remove(mf.FileName)
	if err != nil {
		return fmt.Errorf("error while removing '%v' from GridFS: %v\n", mf.FileName, err)
	}
	log.Logvf(log.Always, "successfully deleted all instances of '%v' from GridFS\n", mf.FileName)
	return nil
}

// logic for deleting a file with 'delete_id'
func (mf *MongoFiles) handleDeleteID(gfs *mgo.GridFS) error {
	id, err := mf.parseID()
	if err != nil {
		return err
	}
	if err = gfs.RemoveId(id); err != nil {
		return fmt.Errorf("error while removing file with _id %v from GridFS: %v\n", mf.Id, err)
	}
	log.Logvf(log.Always, fmt.Sprintf("successfully deleted file with _id %v from GridFS\n", mf.Id))
	return nil
}

// parse and convert extended JSON
func (mf *MongoFiles) parseID() (interface{}, error) {
	// parse the id using extended json
	var asJSON interface{}
	err := json.Unmarshal([]byte(mf.Id), &asJSON)
	if err != nil {
		return nil, fmt.Errorf(
			"error parsing _id as json: %v; make sure you are properly escaping input", err)
	}
	id, err := bsonutil.ConvertJSONValueToBSON(asJSON)
	if err != nil {
		return nil, fmt.Errorf("error converting _id to bson: %v", err)
	}
	return id, nil
}

// writeFile writes a file from gridFS to stdout or the filesystem.
func (mf *MongoFiles) writeFile(gridFile *mgo.GridFile) (err error) {
	localFileName := mf.getLocalFileName(gridFile)
	var localFile io.WriteCloser
	if localFileName == "-" {
		localFile = os.Stdout
	} else {
		if localFile, err = os.Create(localFileName); err != nil {
			return fmt.Errorf("error while opening local file '%v': %v\n", localFileName, err)
		}
		defer localFile.Close()
		log.Logvf(log.DebugLow, "created local file '%v'", localFileName)
	}

	if _, err = io.Copy(localFile, gridFile); err != nil {
		return fmt.Errorf("error while writing data into local file '%v': %v\n", localFileName, err)
	}
	return nil
}

func (mf *MongoFiles) handlePut(gfs *mgo.GridFS, hasID bool) (err error) {
	localFileName := mf.getLocalFileName(nil)

	// check if --replace flag turned on
	if mf.StorageOptions.Replace {
		err = gfs.Remove(mf.FileName)
		if err != nil {
			return err
		}
		// always log that data has been removed
		log.Logvf(log.Always, "removed all instances of '%v' from GridFS\n", mf.FileName)
	}

	var localFile io.ReadCloser

	if localFileName == "-" {
		localFile = os.Stdin
	} else {
		localFile, err = os.Open(localFileName)
		if err != nil {
			return fmt.Errorf("error while opening local file '%v' : %v\n", localFileName, err)
		}
		defer localFile.Close()
		log.Logvf(log.DebugLow, "creating GridFS file '%v' from local file '%v'", mf.FileName, localFileName)
	}

	gridFile, err := gfs.Create(mf.FileName)
	if err != nil {
		return fmt.Errorf("error while creating '%v' in GridFS: %v\n", mf.FileName, err)
	}
	defer func() {
		// GridFS files flush a buffer on Close(), so it's important we
		// capture any errors that occur as this function exits and
		// overwrite the error if earlier writes executed successfully
		if closeErr := gridFile.Close(); err == nil && closeErr != nil {
			log.Logvf(log.DebugHigh, "error occurred while closing GridFS file handler")
			err = fmt.Errorf("error while storing '%v' into GridFS: %v\n", localFileName, closeErr)
		}
	}()

	if hasID {
		id, err := mf.parseID()
		if err != nil {
			return err
		}
		gridFile.SetId(id)
	}

	// set optional mime type
	if mf.StorageOptions.ContentType != "" {
		gridFile.SetContentType(mf.StorageOptions.ContentType)
	}

	n, err := io.Copy(gridFile, localFile)
	if err != nil {
		return fmt.Errorf("error while storing '%v' into GridFS: %v\n", localFileName, err)
	}
	log.Logvf(log.DebugLow, "copied %v bytes to server", n)

	log.Logvf(log.Always, fmt.Sprintf("added file: %v\n", gridFile.Name()))
	return nil
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

	var mode = mgo.Nearest
	var tags bson.D

	if mf.InputOptions.ReadPreference != "" {
		var err error
		mode, tags, err = db.ParseReadPreference(mf.InputOptions.ReadPreference)
		if err != nil {
			return "", fmt.Errorf("error parsing --readPreference : %v", err)
		}
		if len(tags) > 0 {
			mf.SessionProvider.SetTags(tags)
		}
	}

	mf.SessionProvider.SetReadPreference(mode)
	mf.SessionProvider.SetTags(tags)
	mf.SessionProvider.SetFlags(db.DisableSocketTimeout)

	// get session
	session, err := mf.SessionProvider.GetSession()
	if err != nil {
		return "", err
	}
	defer session.Close()

	// check type of node we're connected to, and fall back to w=1 if standalone (for <= 2.4)
	nodeType, err := mf.SessionProvider.GetNodeType()
	if err != nil {
		return "", fmt.Errorf("error determining type of node connected: %v", err)
	}

	log.Logvf(log.DebugLow, "connected to node type: %v", nodeType)

	safety, err := db.BuildWriteConcern(mf.StorageOptions.WriteConcern, nodeType,
		mf.ToolOptions.URI.ParsedConnString())

	if err != nil {
		return "", fmt.Errorf("error parsing write concern: %v", err)
	}

	// configure the session with the appropriate write concern and ensure the
	// socket does not timeout
	session.SetSafe(safety)

	if displayHost {
		log.Logvf(log.Always, "connected to: %v", connUrl)
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

	log.Logvf(log.Info, "handling mongofiles '%v' command...", mf.Command)

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

		err = mf.handleGet(gfs)
		if err != nil {
			return "", err
		}

	case GetID:

		err = mf.handleGetID(gfs)
		if err != nil {
			return "", err
		}

	case Put:

		err = mf.handlePut(gfs, false)
		if err != nil {
			return "", err
		}

	case PutID:

		err = mf.handlePut(gfs, true)
		if err != nil {
			return "", err
		}

	case Delete:

		err = mf.handleDelete(gfs)
		if err != nil {
			return "", err
		}

	case DeleteID:

		err = mf.handleDeleteID(gfs)
		if err != nil {
			return "", err
		}

	}

	return output, nil
}

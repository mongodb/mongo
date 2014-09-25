package db

import (
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/json"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

const MaxBSONSize = 16 * 1024 * 1024

type ShimMode int

const (
	Dump ShimMode = iota
	Insert
	Drop
	Remove
)

type Shim struct {
	DBPath         string
	ShimPath       string
	Journal        bool
	DirectoryPerDB bool
}

func NewShim(dbPath string, directoryPerDb, journal bool) (*Shim, error) {
	shimLoc, err := LocateShim()
	if err != nil {
		return nil, err
	}
	return &Shim{dbPath, shimLoc, journal, directoryPerDb}, nil
}

type ShimDocSource struct {
	stream      *BSONSource
	shimProcess StorageShim
}

func (sds *ShimDocSource) Err() error {
	return sds.stream.Err()
}

func (sds *ShimDocSource) LoadNextInto(into []byte) (bool, int32) {
	return sds.stream.LoadNextInto(into)
}

func (sds *ShimDocSource) Close() (err error) {
	defer func() {
		err2 := sds.shimProcess.WaitResult()
		if err2 == nil {
			err = err2
		}
	}()
	err = sds.stream.Close()
	return
}

func (shim *Shim) Find(DB, Collection string, Skip, Limit int, Query interface{}, Sort []string) (RawDocSource, error) {
	var queryStr string
	if Query == nil {
		queryStr = ""
	} else if queryRaw, ok := Query.(string); ok {
		queryStr = queryRaw
	} else {
		Query, err := bsonutil.ConvertBSONValueToJSON(Query)
		if err != nil {
			return nil, err
		}
		queryBytes, err := json.Marshal(Query)
		if err != nil {
			return nil, err
		}
		queryStr = string(queryBytes)
	}

	queryShim := StorageShim{
		DBPath:         shim.DBPath,
		Database:       DB,
		Collection:     Collection,
		Skip:           Skip,
		Limit:          Limit,
		ShimPath:       shim.ShimPath,
		Query:          queryStr,
		Sort:           Sort,
		Mode:           Dump,
		DirectoryPerDB: shim.DirectoryPerDB,
		Journal:        shim.Journal,
	}
	out, _, err := queryShim.Open()
	if err != nil {
		return nil, err
	}
	return &ShimDocSource{out, queryShim}, nil
}

func (shim *Shim) FindDocs(DB, Collection string, Skip, Limit int, Query interface{}, Sort []string, flags int) (DocSource, error) {
	rds, err := shim.Find(DB, Collection, Skip, Limit, Query, Sort)
	if err != nil {
		return nil, err
	}
	return NewDecodedBSONSource(rds), nil
}

func (shim *Shim) FindOne(DB, Collection string, Skip int, Query interface{}, Sort []string, out interface{}, flags int) error {
	docSource, err := shim.FindDocs(DB, Collection, Skip, 1, Query, Sort, flags)
	if err != nil {
		return err
	}
	defer docSource.Close()
	hasDoc := docSource.Next(out)
	if !hasDoc {
		return docSource.Err()
	}
	return nil
}

func (shim *Shim) OpenInsertStream(DB, Collection string) (DocSink, error) {
	writerShim := &StorageShim{
		DBPath:         shim.DBPath,
		Database:       DB,
		Collection:     Collection,
		ShimPath:       shim.ShimPath,
		Mode:           Insert,
		DirectoryPerDB: shim.DirectoryPerDB,
		Journal:        shim.Journal,
	}
	_, writer, err := writerShim.Open()
	if err != nil {
		return nil, err
	}
	return &EncodedBSONSink{writer, writerShim}, nil
}


func (shim *Shim) Remove(DB, Collection string, Query interface{}) error {
	Query, err := bsonutil.ConvertBSONValueToJSON(Query)
	if err != nil {
		return fmt.Errorf("error converting query '%v' into JSON: %v", Query, err)
	}
	queryBytes, err := json.Marshal(Query)
	if err != nil {
		return fmt.Errorf("error marshaling query '%v' into bytes: %v", Query, err) 
	}
	removerShim := StorageShim{
		DBPath:         shim.DBPath,
		Database:       DB,
		Collection:     Collection,
		ShimPath:       shim.ShimPath,
		Query:          string(queryBytes),
		Mode:           Remove,
		DirectoryPerDB: shim.DirectoryPerDB,
		Journal:        shim.Journal,
	}
	_, _, err = removerShim.Open()
	if err != nil {
		return fmt.Errorf("error opening mongoshim: %v", err)
	}
	err = removerShim.Close()
	if err != nil {
		return fmt.Errorf("error closing mongoshim resource handle: %v", err)
	}
	return nil
}

type databaseNames struct {
	Databases []struct {
		Name  string
		Empty bool
	}
}

func (shim *Shim) CollectionNames(dbName string) (names []string, err error) {
	//TODO handle diff storage engines?
	c := len(dbName) + 1
	iter, err := shim.FindDocs(dbName, "system.namespaces", 0, 0, nil, nil, 0)
	if err != nil {
		return
	}
	defer iter.Close()
	var result *struct{ Name string }
	for iter.Next(&result) {
		if strings.Index(result.Name, "$") < 0 || strings.Index(result.Name, ".oplog.$") >= 0 {
			names = append(names, result.Name[c:])
		}
	}
	sort.Strings(names)
	return names, nil

}

func (shim *Shim) DatabaseNames() ([]string, error) {
	var result databaseNames
	err := shim.Run("listDatabases", &result, "admin")
	if err != nil {
		return nil, err
	}
	names := []string{}
	for _, db := range result.Databases {
		if !db.Empty {
			names = append(names, db.Name)
		}
	}
	return names, nil

}

func (shim *Shim) Run(command interface{}, out interface{}, database string) error {
	if name, ok := command.(string); ok {
		command = bson.M{name: 1}
	}

	commandRaw, err := json.Marshal(command)
	if err != nil {
		return err
	}
	commandShim := StorageShim{
		DBPath:         shim.DBPath,
		Database:       database,
		Collection:     "$cmd",
		Skip:           0,
		Limit:          1,
		ShimPath:       shim.ShimPath,
		Query:          string(commandRaw),
		Mode:           Dump,
		DirectoryPerDB: shim.DirectoryPerDB,
		Journal:        shim.Journal,
	}
	bsonSource, _, err := commandShim.Open()
	if err != nil {
		return err
	}
	decodedResult := NewDecodedBSONSource(bsonSource)
	hasDoc := decodedResult.Next(out)
	if !hasDoc {
		if err := decodedResult.Err(); err != nil {
			return err
		}
		return fmt.Errorf("Didn't receive response from shim with command result.")
	}
	defer commandShim.Close()
	return commandShim.WaitResult()
}

type StorageShim struct {
	DBPath         string
	Database       string
	Collection     string
	Skip           int
	Limit          int
	ShimPath       string
	Query          string
	Sort           []string
	DirectoryPerDB bool
	Journal        bool
	Mode           ShimMode
	shimProcess    *exec.Cmd
	stdin          io.WriteCloser
}

func makeSort(fields []string) bson.D {
	val := bson.D{}
	for _, field := range fields {
		direction := 1
		if strings.HasPrefix(field, "-") {
			direction = -1
			field = field[1:]
		}
		dElem := bson.DocElem{Name: field, Value: direction}
		val = append(val, dElem)
	}
	return val
}

func buildArgs(shim StorageShim) ([]string, error) {
	returnVal := []string{}
	if shim.DBPath != "" {
		returnVal = append(returnVal, "--dbpath", shim.DBPath)
	}
	if shim.Journal {
		returnVal = append(returnVal, "--journal")
	}
	if shim.DirectoryPerDB {
		returnVal = append(returnVal, "--directoryperdb")
	}
	if shim.Database != "" {
		returnVal = append(returnVal, "-d", shim.Database)
	}
	if shim.Collection != "" {
		returnVal = append(returnVal, "-c", shim.Collection)
	}
	if shim.Limit > 0 {
		returnVal = append(returnVal, "--limit", fmt.Sprintf("%v", shim.Limit))
	}
	if shim.Skip > 0 {
		returnVal = append(returnVal, "--skip", fmt.Sprintf("%v", shim.Skip))
	}
	if len(shim.Sort) > 0 {
		sortObj := bsonutil.MarshalD(makeSort(shim.Sort))
		sortObjJson, err := json.Marshal(sortObj)
		if err != nil {
			return nil, nil
		}
		returnVal = append(returnVal, "--sort", string(sortObjJson))

	}

	if shim.Mode != Drop && shim.Query != "" {
		returnVal = append(returnVal, "--query", shim.Query)
	}

	/*
		if shim.Mode == Dump {
			returnVal = append(returnVal, "--query", shim.Sort)
		}
	*/

	switch shim.Mode {
	case Dump:
	case Insert:
		returnVal = append(returnVal, "--load")
	case Drop:
		returnVal = append(returnVal, "--drop")
	case Remove:
		returnVal = append(returnVal, "--remove")
	}
	return returnVal, nil
}

func checkExists(path string) (bool, error) {
	if _, err := os.Stat(path); err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}

var ErrShimNotFound = errors.New("Shim not found")

func LocateShim() (string, error) {
	shimLoc := os.Getenv("MONGOSHIM")
	if shimLoc == "" {
		dir, err := filepath.Abs(filepath.Dir(os.Args[0]))
		if err != nil {
			return "", err
		}
		shimLoc = filepath.Join(dir, "mongoshim")
		if runtime.GOOS == "windows" {
			shimLoc += ".exe"
		}
	}

	if shimLoc == "" {
		return "", ErrShimNotFound
	}

	exists, err := checkExists(shimLoc)
	if err != nil {
		return "", err
	}
	if !exists {
		return "", ErrShimNotFound
	}
	return shimLoc, nil
}

//Open() starts the external shim process and returns an instance of BSONSource
//bound to its STDOUT stream.
func (shim *StorageShim) Open() (*BSONSource, *BSONSink, error) {
	args, err := buildArgs(*shim)
	if err != nil {
		return nil, nil, err
	}
	cmd := exec.Command(shim.ShimPath, args...)
	stdOut, err := cmd.StdoutPipe()
	if err != nil {
		return nil, nil, err
	}
	stdErr, err := cmd.StderrPipe()
	if err != nil {
		return nil, nil, err
	}
	go func() {
		io.Copy(os.Stderr, stdErr)
	}()

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, nil, err
	}
	shim.stdin = stdin

	err = cmd.Start()
	if err != nil {
		return nil, nil, err
	}
	shim.shimProcess = cmd

	return &BSONSource{stdOut, nil}, &BSONSink{stdin}, nil
}

func (shim *StorageShim) WaitResult() error {
	if shim.shimProcess != nil {
		err := shim.shimProcess.Wait()
		return err
	}
	return nil
}

func (shim *StorageShim) Close() error {
	if shim.shimProcess != nil && shim.shimProcess.Process != nil {
		shim.stdin.Close()
		_, err := shim.shimProcess.Process.Wait()
		return err
	}
	return nil
}

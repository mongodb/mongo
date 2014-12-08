package db

import (
	"fmt"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"strconv"
	"strings"
)

const (
	Snapshot = 1 << iota
	LogReplay
	Prefetch
)

type WriteCommandResponse struct {
	NumAffected       *int                `bson:"n"`
	Ok                int                 `bson:"ok"`
	WriteErrors       []WriteCommandError `bson:"writeErrors"`
	WriteConcernError WriteCommandError   `bson:"writeConcernErrors"`
}

type WriteCommandError struct {
	Index  int    `bson:"index"`
	Code   int    `bson:"code"`
	Errmsg string `bson:"errmsg"`
}

type CommandRunner interface {
	Run(command interface{}, out interface{}, database string) error
	FindDocs(DB, Collection string, Skip, Limit int, Query interface{}, Sort []string, opts int) (DocSource, error)
	FindOne(DB, Collection string, Skip int, Query interface{}, Sort []string, into interface{}, opts int) error
	Remove(DB, Collection string, Query interface{}) error
	DatabaseNames() ([]string, error)
	CollectionNames(dbName string) ([]string, error)
}

func (sp *SessionProvider) Remove(DB, Collection string, Query interface{}) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	_, err = session.DB(DB).C(Collection).RemoveAll(Query)
	return err
}

func (sp *SessionProvider) Run(command interface{}, out interface{}, database string) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()
	return session.DB(database).Run(command, out)
}

func (sp *SessionProvider) DatabaseNames() ([]string, error) {
	session, err := sp.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()
	return session.DatabaseNames()
}

func (sp *SessionProvider) CollectionNames(dbName string) ([]string, error) {
	session, err := sp.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()
	return session.DB(dbName).CollectionNames()
}

func (sp *SessionProvider) FindDocs(DB, Collection string, Skip, Limit int, Query interface{}, Sort []string, flags int) (DocSource, error) {
	session, err := sp.GetSession()
	if err != nil {
		return nil, err
	}
	q := session.DB(DB).C(Collection).Find(Query).Sort(Sort...).Skip(Skip).Limit(Limit)
	q = ApplyFlags(q, session, flags)
	return &CursorDocSource{q.Iter(), session}, nil
}

func (sp *SessionProvider) IsReplicaSet() (bool, error) {
	session, err := sp.GetSession()
	if err != nil {
		return false, err
	}
	defer session.Close()
	masterDoc := bson.M{}
	err = session.Run("isMaster", &masterDoc)
	if err != nil {
		return false, err
	}
	_, hasSetName := masterDoc["setName"]
	_, hasHosts := masterDoc["hosts"]
	return hasSetName || hasHosts, nil
}

// IsMongos() returns a boolean representing whether the connected server
// is a mongos along with any error that occurs.
func (sp *SessionProvider) IsMongos() (bool, error) {
	session, err := sp.GetSession()
	if err != nil {
		return false, err
	}
	defer session.Close()
	masterDoc := struct {
		Msg string `bson:"msg"`
	}{}
	if err = session.Run("isMaster", &masterDoc); err != nil {
		return false, err
	}
	// isdbgrid is always the msg value when calling isMaster on a mongos
	// see http://docs.mongodb.org/manual/core/sharded-cluster-query-router/
	return masterDoc.Msg == "isdbgrid", nil
}

// SupportsRepairCursor takes in an example db and collection name and
// returns true if the connected server supports the repairCursor command.
// It returns false and the error that occurred if it is not supported.
func (sp *SessionProvider) SupportsRepairCursor(db, collection string) (bool, error) {
	session, err := sp.GetSession()
	if err != nil {
		return false, err
	}
	defer session.Close()

	// This check is slightly hacky, but necessary to allow users to run repair without
	// permissions to all collections. There are multiple reasons a repair command could fail,
	// but we are only interested in the ones that imply that the repair command is not
	// usable by the connected server. If we do not get one of these specific error messages,
	// we will let the error happen again later.
	repairIter := session.DB(db).C(collection).Repair()
	repairIter.Next(bson.D{})
	err = repairIter.Err()
	if err == nil {
		return true, nil
	}
	if strings.Index(err.Error(), "no such cmd: repairCursor") > -1 {
		// return a helpful error message for early server versions
		return false, fmt.Errorf(
			"--repair flag cannot be used on mongodb versions before 2.7.8.")
	}
	if strings.Index(err.Error(), "repair iterator not supported") > -1 {
		// helpful error message if the storage engine does not support repair (WiredTiger)
		return false, fmt.Errorf("--repair is not supported by the connected storage engine")
	}

	return true, nil
}

func (sp *SessionProvider) SupportsWriteCommands() (bool, error) {
	session, err := sp.GetSession()
	if err != nil {
		return false, err
	}
	defer session.Close()
	masterDoc := bson.M{}
	err = session.Run("isMaster", &masterDoc)
	if err != nil {
		return false, err
	}
	dbOkValue, hasOk := masterDoc["ok"]
	dbMinWireVersion, hasMinWireVersion := masterDoc["minWireVersion"]
	dbMaxWireVersion, hasMaxWireVersion := masterDoc["maxWireVersion"]
	minWireVersion, ok := dbMinWireVersion.(int)
	if !ok {
		return false, nil
	}
	maxWireVersion, ok := dbMaxWireVersion.(int)
	if !ok {
		return false, nil
	}
	var okValue int
	switch okValueType := dbOkValue.(type) {
	case int:
		okValue = dbOkValue.(int)
	case float64:
		okValue = int(dbOkValue.(float64))
	case string:
		okValue, err = strconv.Atoi(dbOkValue.(string))
		if err != nil {
			return false, fmt.Errorf("expected int for ok value, got '%v' (%v)", dbOkValue, okValueType)
		}
	default:
		return false, fmt.Errorf("expected int for ok value, got '%v' (%v)", dbOkValue, okValueType)
	}
	// the connected server supports write commands if its minWireVersion <= 2
	// and its maxWireVersion >= 2
	if hasOk && okValue == 1 &&
		hasMinWireVersion && hasMaxWireVersion &&
		minWireVersion <= 2 && 2 <= maxWireVersion {
		return true, nil
	}
	return false, nil
}

func (sp *SessionProvider) FindOne(DB, Collection string, Skip int, Query interface{}, Sort []string, into interface{}, flags int) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	q := session.DB(DB).C(Collection).Find(Query).Sort(Sort...).Skip(Skip)
	q = ApplyFlags(q, session, flags)
	return q.One(into)
}

func ApplyFlags(q *mgo.Query, session *mgo.Session, flags int) *mgo.Query {
	if flags&Snapshot > 0 {
		q = q.Snapshot()
	}
	if flags&LogReplay > 0 {
		q = q.Snapshot()
	}
	if flags&Prefetch > 0 {
		session.SetPrefetch(1.0)
	}
	return q
}

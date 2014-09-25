package db

import "gopkg.in/mgo.v2"

const (
	Snapshot = 1 << iota
	LogReplay
	Prefetch
)

type CommandRunner interface {
	Run(command interface{}, out interface{}, database string) error

	FindDocs(DB, Collection string, Skip, Limit int, Query interface{}, Sort []string, opts int) (DocSource, error)
	FindOne(DB, Collection string, Skip int, Query interface{}, Sort []string, into interface{}, opts int) error
	
	OpenInsertStream(DB, Collection string) (DocSink, error)
	
	Remove(DB, Collection string, Query interface{}) error
	
	DatabaseNames() ([]string, error)
	CollectionNames(dbName string) ([]string, error)
}

func (sp *SessionProvider) OpenInsertStream(DB, Collection string) (DocSink, error) {
	session, err := sp.GetSession()
	if err != nil {
		return nil, err
	}
	
	coll := session.DB(DB).C(Collection)
	return &CollectionSink{coll, session}, nil
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
	q = applyFlags(q, session, flags)
	return &CursorDocSource{q.Iter(), session}, nil
}

func (sp *SessionProvider) FindOne(DB, Collection string, Skip int, Query interface{}, Sort []string, into interface{}, flags int) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	q := session.DB(DB).C(Collection).Find(Query).Sort(Sort...).Skip(Skip)
	q = applyFlags(q, session, flags)
	return q.One(into)
}

func applyFlags(q *mgo.Query, session *mgo.Session, flags int) *mgo.Query {
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

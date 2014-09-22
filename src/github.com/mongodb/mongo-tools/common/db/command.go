package db

type CommandRunner interface {
	Run(command interface{}, out interface{}, database string) error
	//Find(DB, Collection string, Skip, Limit int, Query interface{}, Sort interface{}) (RawDocSource, error)
	FindDocs(DB, Collection string, Skip, Limit int, Query interface{}, Sort []string) (DocSource, error)
	FindOne(DB, Collection string, Skip int, Query interface{}, Sort []string, into interface{}) error
	DatabaseNames() ([]string, error)
	CollectionNames(dbName string) ([]string, error)
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

func (sp *SessionProvider) FindDocs(DB, Collection string, Skip, Limit int, Query interface{}, Sort []string) (DocSource, error) {
	session, err := sp.GetSession()
	if err != nil {
		return nil, err
	}
	//TODO sort!!!!
	q := session.DB(DB).C(Collection).Find(Query).Sort(Sort...).Skip(Skip).Limit(Limit)
	return &CursorDocSource{q.Iter(), session}, nil
}

func (sp *SessionProvider) FindOne(DB, Collection string, Skip int, Query interface{}, Sort []string, into interface{}) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()
	//TODO sort!!!!
	err = session.DB(DB).C(Collection).Find(Query).Sort(Sort...).Skip(Skip).One(into)
	return err
}

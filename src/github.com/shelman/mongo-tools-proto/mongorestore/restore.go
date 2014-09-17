package mongorestore

import (
	"github.com/shelman/mongo-tools-proto/common/db"
)

func (restore *MongoRestore) RestoreIntents() error {
	for intent := restore.manager.Pop(); intent != nil; intent = restore.manager.Pop() {
		log.Logf(0, "%+v", intent)
	}
	return nil
}

func (restore *MongoRestore) RestoreCollectionToDB(dbName, colName string,
	decoder *db.DecodedBSONSource) error {

	session, err := restore.SessionProvider.GetSession()
	if err != nil {
		return fmt.Printf("can't esablish session: %v", err)
	}
	s.SetSafe(nil)
	defer s.Close()
	c := s.DB().C("")

	stream := &BSONSource{Stream: r}
	doc := make([]byte, 1024*1024*16) //TODO pools
	for {
		good, n := stream.LoadNextInto(doc)
		if !good {
			break
		}
		err := c.Insert(bson.Raw{Data: doc[0:n]})
		if err != nil {
			fmt.Println(err)
			break
		}
	}
}

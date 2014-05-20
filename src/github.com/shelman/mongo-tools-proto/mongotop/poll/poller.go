package poll

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	"github.com/shelman/mongo-tools-proto/mongotop/result"
)

// responsible for polling for results
type Poller interface {
	Connect() (string, error)
	Poll() (*result.TopResults, error)
}

// hits the db directly
type DBPoller struct {
}

func (self *DBPoller) Connect() (string, error) {
	return db.Url(), db.ConfirmConnect()
}

func (self *DBPoller) Poll() (*result.TopResults, error) {

	// get a db session
	session, err := db.GetSession()
	if err != nil {
		return nil, fmt.Errorf("error making db connection: %v", err)
	}
	defer session.Close()

	// get the admin db
	adminDB := session.DB("admin")
	res := &result.TopResults{}

	// run the command
	if err := adminDB.Run("top", res); err != nil {
		return nil, fmt.Errorf("error running top cmd: %v", err)
	}

	// success
	return res, nil
}

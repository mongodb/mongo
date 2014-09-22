package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongorestore/options"
	"gopkg.in/mgo.v2"
)

type MongoRestore struct {
	ToolOptions   *commonopts.ToolOptions
	InputOptions  *options.InputOptions
	OutputOptions *options.OutputOptions

	SessionProvider *db.SessionProvider

	TargetDirectory string

	// other internal state
	manager *IntentManager
	safety  *mgo.Safe
}

func (restore *MongoRestore) Restore() error {
	// TODO validate options

	// 1. Build up all intents to be restored
	restore.manager = NewIntentManager()

	var err error
	switch {
	case restore.ToolOptions.DB == "" && restore.ToolOptions.Collection == "":
		log.Logf(0,
			"building a list of dbs and collections to restore from %v dir",
			restore.TargetDirectory)
		err = restore.CreateAllIntents(restore.TargetDirectory)
	case restore.ToolOptions.DB != "" && restore.ToolOptions.Collection == "":
		log.Logf(0,
			"building a list of collections to restore from %v dir",
			restore.TargetDirectory)
		err = restore.CreateIntentsForDB(
			restore.ToolOptions.DB,
			restore.TargetDirectory)
	case restore.ToolOptions.DB != "" && restore.ToolOptions.Collection != "":
		log.Logf(0, "checking for collection data in %v", restore.TargetDirectory)
		err = restore.CreateIntentForCollection(
			restore.ToolOptions.DB,
			restore.ToolOptions.Collection,
			restore.TargetDirectory)
	}
	if err != nil {
		return fmt.Errorf("error scanning filesystem: %v", err)
	}

	// 2. Restore them...
	err = restore.RestoreIntents()
	if err != nil {
		return fmt.Errorf("restore error: %v", err)
	}
	log.Log(0, "done")

	return nil
}

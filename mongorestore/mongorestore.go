package mongorestore

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongorestore/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"sync"
)

type MongoRestore struct {
	ToolOptions   *commonopts.ToolOptions
	InputOptions  *options.InputOptions
	OutputOptions *options.OutputOptions

	SessionProvider *db.SessionProvider

	TargetDirectory string

	tempUsersCol string
	tempRolesCol string

	// other internal state
	manager         *intents.Manager
	safety          *mgo.Safe
	progressManager *progress.Manager

	objCheck   bool
	oplogLimit bson.MongoTimestamp
	useStdin   bool
	isMongos   bool

	// a map of database names to a list of collection names
	knownCollections      map[string][]string
	knownCollectionsMutex sync.Mutex
}

func (restore *MongoRestore) ParseAndValidateOptions() error {
	// Can't use option pkg defaults for --objcheck because it's two separate flags,
	// and we need to be able to see if they're both being used. We default to
	// true here and then see if noobjcheck is enabled.
	log.Log(log.DebugHigh, "checking options")
	if restore.InputOptions.Objcheck {
		restore.objCheck = true
		log.Log(log.DebugHigh, "\tdumping with object check enabled")
	} else {
		log.Log(log.DebugHigh, "\tdumping with object check disabled")
	}

	if restore.ToolOptions.DB == "" && restore.ToolOptions.Collection != "" {
		return fmt.Errorf("cannot dump a collection without a specified database")
	}

	if restore.ToolOptions.DB != "" {
		if err := util.ValidateDBName(restore.ToolOptions.DB); err != nil {
			return fmt.Errorf("invalid db name: %v", err)
		}
	}
	if restore.ToolOptions.Collection != "" {
		if err := util.ValidateCollectionGrammar(restore.ToolOptions.Collection); err != nil {
			return fmt.Errorf("invalid collection name: %v", err)
		}
	}
	if restore.InputOptions.RestoreDBUsersAndRoles && restore.ToolOptions.DB == "" {
		return fmt.Errorf("cannot use --restoreDbUsersAndRoles without a specified database")
	}
	if restore.InputOptions.RestoreDBUsersAndRoles && restore.ToolOptions.DB == "admin" {
		return fmt.Errorf("cannot use --restoreDbUsersAndRoles with the admin database")
	}

	var err error
	restore.isMongos, err = restore.SessionProvider.IsMongos()
	if err != nil {
		return err
	}
	if restore.isMongos {
		log.Log(log.DebugLow, "restoring to a sharded system")
		if restore.ToolOptions.DB == "" {
			return fmt.Errorf("cannot do a full restore on a sharded system")
		}
	}

	if restore.InputOptions.OplogLimit != "" {
		if !restore.InputOptions.OplogReplay {
			return fmt.Errorf("cannot use --oplogLimit without --oplogReplay enabled")
		}
		restore.oplogLimit, err = ParseTimestampFlag(restore.InputOptions.OplogLimit)
		if err != nil {
			return fmt.Errorf("error parsing timestamp argument to --oplogLimit: %v", err)
		}
	}

	// check if we are using a replica set and fall back to w=1 if we aren't (for <= 2.4)
	isRepl, err := restore.SessionProvider.IsReplicaSet()
	if err != nil {
		return fmt.Errorf("error determining if connected to replica set: %v", err)
	}
	restore.safety, err = util.BuildWriteConcern(restore.OutputOptions.WriteConcern, isRepl)
	if err != nil {
		return fmt.Errorf("error parsing write concern: %v", err)
	}

	if restore.tempUsersCol == "" {
		restore.tempUsersCol = "tempusers"
	}
	if restore.tempRolesCol == "" {
		restore.tempRolesCol = "temproles"
	}

	if restore.ToolOptions.HiddenOptions.BulkWriters < 0 {
		return fmt.Errorf(
			"cannot specify a negative number of insertion workers per collection")
	}

	// a single dash signals reading from stdin
	if restore.TargetDirectory == "-" {
		restore.useStdin = true
		if restore.ToolOptions.Collection == "" {
			return fmt.Errorf("cannot restore from stdin without a specified collection")
		}
	}

	return nil
}

func (restore *MongoRestore) Restore() error {
	err := restore.ParseAndValidateOptions()
	if err != nil {
		log.Logf(log.DebugLow, "got error from options parsing: %v", err)
		return err
	}

	// 1. Build up all intents to be restored
	restore.manager = intents.NewCategorizingIntentManager()

	switch {
	case restore.ToolOptions.DB == "" && restore.ToolOptions.Collection == "":
		log.Logf(log.Always,
			"building a list of dbs and collections to restore from %v dir",
			restore.TargetDirectory)
		err = restore.CreateAllIntents(restore.TargetDirectory)
	case restore.ToolOptions.DB != "" && restore.ToolOptions.Collection == "":
		log.Logf(log.Always,
			"building a list of collections to restore from %v dir",
			restore.TargetDirectory)
		err = restore.CreateIntentsForDB(
			restore.ToolOptions.DB,
			restore.TargetDirectory)
	case restore.ToolOptions.DB != "" && restore.ToolOptions.Collection != "":
		log.Logf(log.Always, "checking for collection data in %v", restore.TargetDirectory)
		err = restore.CreateIntentForCollection(
			restore.ToolOptions.DB,
			restore.ToolOptions.Collection,
			restore.TargetDirectory)
	}
	if err != nil {
		return fmt.Errorf("error scanning filesystem: %v", err)
	}

	// 2. Restore them...
	if restore.OutputOptions.NumParallelCollections > 0 {
		restore.manager.Finalize(intents.MultiDatabaseLTF)
	} else {
		// use legacy restoration order if we are single-threaded
		restore.manager.Finalize(intents.Legacy)
	}
	err = restore.RestoreIntents()
	if err != nil {
		return fmt.Errorf("restore error: %v", err)
	}

	// 3. Restore users/roles
	// TODO comment all cases
	if restore.InputOptions.RestoreDBUsersAndRoles || restore.ToolOptions.DB == "" || restore.ToolOptions.DB == "admin" {
		if restore.manager.Users() != nil {
			err = restore.RestoreUsersOrRoles(Users, restore.manager.Users())
			if err != nil {
				return fmt.Errorf("restore error: %v", err)
			}
		}
		if restore.manager.Roles() != nil {
			err = restore.RestoreUsersOrRoles(Roles, restore.manager.Roles())
			if err != nil {
				return fmt.Errorf("restore error: %v", err)
			}
		}
	}

	// 4. Restore oplog
	if restore.InputOptions.OplogReplay {
		err = restore.RestoreOplog()
		if err != nil {
			return fmt.Errorf("restore error: %v", err)
		}
	}

	log.Log(log.Always, "done")
	return nil
}

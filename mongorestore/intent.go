package mongorestore

import (
	"github.com/mongodb/mongo-tools/common/log"
	"sync"
)

// TODO: make this reusable for dump?

// mongorestore first scans the directory to generate a list
// of all files to restore and what they map to. TODO comments
type Intent struct {
	// Namespace info
	DB string
	C  string

	// File locations as absolute paths
	BSONPath     string
	MetadataPath string

	// File size, for some prioritizer implementations.
	BSONSize int64
}

func (it *Intent) Key() string {
	return it.DB + "." + it.C
}

func (it *Intent) IsOplog() bool {
	return it.DB == "" && it.C == "oplog"
}

func (it *Intent) IsUsers() bool {
	if it.C == "$admin.system.users" {
		return true
	}
	if it.DB == "admin" && it.C == "system.users" {
		return true
	}
	return false
}

func (it *Intent) IsRoles() bool {
	if it.C == "$admin.system.roles" {
		return true
	}
	if it.DB == "admin" && it.C == "system.roles" {
		return true
	}
	return false
}

func (it *Intent) IsSystemIndexes() bool {
	return it.C == "system.indexes" && it.BSONPath != ""
}

// Intent Manager

type IntentManager struct {
	// map for merging metadata with BSON intents
	intents map[string]*Intent

	// legacy mongorestore works in the order that paths are discovered,
	// so we need an ordered data structure to preserve this behavior.
	intentsByDiscoveryOrder []*Intent

	// we need different scheduling order depending on the target
	// mongod/mongos and whether or not we are multi threading;
	// the IntentPrioritizer interface encapsulates this.
	prioritizer     IntentPrioritizer
	priotitizerLock *sync.Mutex

	// special cases that should be saved but not be part of the queue.
	// used to deal with oplog and user/roles restoration, which are
	// handled outside of the basic logic of the tool
	oplogIntent  *Intent
	usersIntent  *Intent
	rolesIntent  *Intent
	indexIntents map[string]*Intent
}

func NewIntentManager() *IntentManager {
	return &IntentManager{
		intents:                 map[string]*Intent{},
		intentsByDiscoveryOrder: []*Intent{},
		indexIntents:            map[string]*Intent{},
		priotitizerLock:         &sync.Mutex{},
	}
}

// Put inserts an intent into the manager. Intents for the same collection
// are merged together, so that BSON and metadata files for the same collection
// are returned in the same intent.
func (manager *IntentManager) Put(intent *Intent) {
	if intent == nil {
		panic("cannot insert nil *Intent into IntentManager")
	}

	if intent.IsOplog() {
		manager.oplogIntent = intent
		return
	}
	if intent.IsSystemIndexes() {
		manager.indexIntents[intent.DB] = intent
		return
	}
	if intent.IsUsers() {
		if intent.BSONPath != "" { //TODO(erf) make this elegant
			manager.usersIntent = intent
		}
		return
	}
	if intent.IsRoles() {
		if intent.BSONPath != "" {
			manager.rolesIntent = intent
		}
		return
	}

	// BSON and metadata files for the same collection are merged
	// into the same intent. This is done to allow for simple
	// pairing of BSON + metadata without keeping track of the
	// state of the filepath walker
	if existing := manager.intents[intent.Key()]; existing != nil {
		// merge new intent into old intent
		if existing.BSONPath == "" {
			existing.BSONPath = intent.BSONPath
		}
		if existing.BSONSize == 0 {
			existing.BSONSize = intent.BSONSize
		}
		if existing.MetadataPath == "" {
			existing.MetadataPath = intent.MetadataPath
		}
		return
	}

	// if key doesn't already exist, add it to the manager
	manager.intents[intent.Key()] = intent
	manager.intentsByDiscoveryOrder = append(manager.intentsByDiscoveryOrder, intent)
}

// Pop returns the next available intent from the manager. If the manager is
// empty, it returns nil. Pop is thread safe.
func (manager *IntentManager) Pop() *Intent {
	manager.priotitizerLock.Lock()
	defer manager.priotitizerLock.Unlock()

	intent := manager.prioritizer.Get()
	return intent
}

// Finish tells the prioritizer that mongorestore is done restoring
// the given collection intent.
func (manager *IntentManager) Finish(intent *Intent) {
	manager.priotitizerLock.Lock()
	defer manager.priotitizerLock.Unlock()
	manager.prioritizer.Finish(intent)
}

// Oplog returns the intent representing the oplog, which isn't
// stored with the other intents, because it is dumped and restored in
// a very different way from other collections.
func (manager *IntentManager) Oplog() *Intent {
	return manager.oplogIntent
}

// SystemIndexes returns the system.indexes bson for a database
func (manager *IntentManager) SystemIndexes(dbName string) *Intent {
	return manager.indexIntents[dbName]
}

// Users returns the intent of the users collection to restore, a special case
func (manager *IntentManager) Users() *Intent {
	return manager.usersIntent
}

// Roles returns the intent of the user roles collection to restore, a special case
func (manager *IntentManager) Roles() *Intent {
	return manager.rolesIntent
}

// Finalize processes the intents for prioritization. Currently only two
// kinds of prioritizers are supported. No more "Put" operations may be done
// after finalize is called.
func (manager *IntentManager) Finalize(pType PriorityType) {
	switch pType {
	case Legacy:
		log.Log(3, "finalizing intent manager with legacy prioritizer")
		manager.prioritizer = NewLegacyPrioritizer(manager.intentsByDiscoveryOrder)
	case MultiDatabaseLTF:
		log.Log(3, "finalizing intent manager with multi-database largest task first prioritizer")
		manager.prioritizer = NewMultiDatabaseLTFPrioritizer(manager.intentsByDiscoveryOrder)
	default:
		panic("cannot initialize IntentPrioritizer with unknown type")
	}
	// release these for the garbage collector and to ensure code correctness
	manager.intents = nil
	manager.intentsByDiscoveryOrder = nil
}

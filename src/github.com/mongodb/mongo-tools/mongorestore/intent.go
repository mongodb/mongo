package mongorestore

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
}

func (it *Intent) Key() string {
	return it.DB + "." + it.C
}

func (it *Intent) IsOplog() bool {
	return it.DB == "" && it.C == "oplog"
}

// Intent Manager
// TODO make this an interface, for testing ease

type IntentManager struct {
	// map for merging metadata w/ bson intents
	intents map[string]*Intent

	// need an ordered list to preserve search order / legacy behavior
	queue []*Intent

	// special cases that go first and last
	oplogIntent         *Intent
	usersAndRolesIntent *Intent
}

func NewIntentManager() *IntentManager {
	return &IntentManager{
		intents: map[string]*Intent{},
		queue:   []*Intent{},
	}
}

func (im *IntentManager) Put(intent *Intent) {
	if intent == nil {
		panic("cannot insert nil *Intent into IntentManager")
	}

	if intent.IsOplog() {
		im.oplogIntent = intent
		return
	}

	// TODO usersAndRoles???

	if existing := im.intents[intent.Key()]; existing != nil {
		// merge new intent into old intent
		if existing.BSONPath == "" {
			existing.BSONPath = intent.BSONPath
		}
		if existing.MetadataPath == "" {
			existing.MetadataPath = intent.MetadataPath
		}
		return
	}

	// if key doesn't already exist, add it to the manager
	im.intents[intent.Key()] = intent
	im.queue = append(im.queue, intent)
}

func (im *IntentManager) Pop() *Intent {
	var intent *Intent

	if len(im.queue) == 0 {
		return nil
	}

	intent, im.queue = im.queue[0], im.queue[1:]
	delete(im.intents, intent.Key())

	return intent
}

func (im *IntentManager) Oplog() *Intent {
	return im.oplogIntent
}

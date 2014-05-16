package result

import ()

// the top results returned from the database
type TopResults struct {
	// namespace -> namespace-specific top info
	Totals map[string]NSTopInfo `bson:"totals"`
}

// top info about a single namespace
type NSTopInfo struct {
	Total TopField `bson:"total"`
	Read  TopField `bson:"readLock"`
	Write TopField `bson:"writeLock"`
}

// a single piece of info within a namespace
type TopField struct {
	Time int `bson:"time"`
}

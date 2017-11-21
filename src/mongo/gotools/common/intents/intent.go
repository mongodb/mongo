// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package intents provides utilities for performing dump/restore operations.
package intents

import (
	"fmt"
	"io"

	"github.com/mongodb/mongo-tools/common"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
)

type file interface {
	io.ReadWriteCloser
	Open() error
	Pos() int64
}

// DestinationConflictError occurs when multiple namespaces map to the same
// destination.
type DestinationConflictError struct {
	Src, Dst string
}

func (e DestinationConflictError) Error() string {
	return fmt.Sprintf("destination conflict: %s (src) => %s (dst)", e.Src, e.Dst)
}

// FileNeedsIOBuffer is an interface that denotes that a struct needs
// an IO buffer that is managed by an outside control. This interface
// is used to both hand off a buffer to a struct and signal that it should
// release its buffer. Added to reduce memory usage as outlined in TOOLS-1088.
type FileNeedsIOBuffer interface {
	TakeIOBuffer([]byte)
	ReleaseIOBuffer()
}

// mongorestore first scans the directory to generate a list
// of all files to restore and what they map to. TODO comments
type Intent struct {
	// Destination namespace info
	DB string
	C  string

	// File locations as absolute paths
	BSONFile     file
	BSONSize     int64
	MetadataFile file

	// Indicates where the intent will be read from or written to
	Location         string
	MetadataLocation string

	// Collection options
	Options *bson.D

	// UUID (for MongoDB 3.6+) as a big-endian hex string
	UUID string

	// File/collection size, for some prioritizer implementations.
	// Units don't matter as long as they are consistent for a given use case.
	Size int64
}

func (it *Intent) Namespace() string {
	return it.DB + "." + it.C
}

func (it *Intent) IsOplog() bool {
	if it.DB == "" && it.C == "oplog" {
		return true
	}
	return it.DB == "local" && (it.C == "oplog.rs" || it.C == "oplog.$main")
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

func (it *Intent) IsAuthVersion() bool {
	if it.C == "$admin.system.version" {
		return true
	}
	if it.DB == "admin" && it.C == "system.version" {
		return true
	}
	return false
}

func (it *Intent) IsSystemIndexes() bool {
	return it.C == "system.indexes"
}

func (intent *Intent) IsSpecialCollection() bool {
	return intent.IsSystemIndexes() || intent.IsUsers() || intent.IsRoles() || intent.IsAuthVersion()
}

func (it *Intent) IsView() bool {
	if it.Options == nil {
		return false
	}
	_, isView := it.Options.Map()["viewOn"]
	return isView
}

func (existing *Intent) MergeIntent(intent *Intent) {
	// merge new intent into old intent
	if existing.BSONFile == nil {
		existing.BSONFile = intent.BSONFile
	}
	if existing.Size == 0 {
		existing.Size = intent.Size
	}
	if existing.Location == "" {
		existing.Location = intent.Location
	}
	if existing.MetadataFile == nil {
		existing.MetadataFile = intent.MetadataFile
	}
	if existing.MetadataLocation == "" {
		existing.MetadataLocation = intent.MetadataLocation
	}

}

type Manager struct {
	// intents are for all of the regular user created collections
	intents map[string]*Intent
	// special intents are for all of the collections that are created by mongod
	// and require special handling
	specialIntents map[string]*Intent

	// legacy mongorestore works in the order that paths are discovered,
	// so we need an ordered data structure to preserve this behavior.
	intentsByDiscoveryOrder []*Intent

	// we need different scheduling order depending on the target
	// mongod/mongos and whether or not we are multi threading;
	// the IntentPrioritizer interface encapsulates this.
	prioritizer IntentPrioritizer

	// special cases that should be saved but not be part of the queue.
	// used to deal with oplog and user/roles restoration, which are
	// handled outside of the basic logic of the tool
	oplogIntent   *Intent
	usersIntent   *Intent
	rolesIntent   *Intent
	versionIntent *Intent
	indexIntents  map[string]*Intent

	// Tells the manager if it should choose a single oplog when multiple are provided.
	smartPickOplog bool

	// Indicates if an the manager has seen two conflicting oplogs.
	oplogConflict bool

	// prevent conflicting destinations by checking which sources map to the
	// same namespace
	destinations map[string][]string
}

func NewIntentManager() *Manager {
	return &Manager{
		intents:                 map[string]*Intent{},
		specialIntents:          map[string]*Intent{},
		intentsByDiscoveryOrder: []*Intent{},
		indexIntents:            map[string]*Intent{},
		smartPickOplog:          false,
		oplogConflict:           false,
		destinations:            map[string][]string{},
	}
}

func (mgr *Manager) SetSmartPickOplog(smartPick bool) {
	mgr.smartPickOplog = smartPick
}

// HasConfigDBIntent returns a bool indicating if any of the intents refer to the "config" database.
// This can be used to check for possible unwanted conflicts before restoring to a sharded system.
func (mgr *Manager) HasConfigDBIntent() bool {
	for _, intent := range mgr.intentsByDiscoveryOrder {
		if intent.DB == "config" {
			return true
		}
	}
	return false
}

// PutOplogIntent takes an intent for an oplog and stores it in the intent manager with the
// provided key. If the manager has smartPickOplog enabled, then it uses a priority system
// to determine which oplog intent to maintain as the actual oplog.
func (manager *Manager) PutOplogIntent(intent *Intent, managerKey string) {
	if manager.smartPickOplog {
		if existing := manager.specialIntents[managerKey]; existing != nil {
			existing.MergeIntent(intent)
			return
		}
		if manager.oplogIntent == nil {
			// If there is no oplog intent, make this one the oplog.
			manager.oplogIntent = intent
			manager.specialIntents[managerKey] = intent
		} else if intent.DB == "" {
			// We already have an oplog and this is a top priority oplog.
			if manager.oplogIntent.DB == "" {
				// If the manager's current oplog is also top priority, we have a
				// conflict and ignore this oplog.
				manager.oplogConflict = true
			} else {
				// If the manager's current oplog is lower priority, replace it and
				// move that one to be a normal intent.
				manager.putNormalIntent(manager.oplogIntent)
				delete(manager.specialIntents, manager.oplogIntent.Namespace())
				manager.oplogIntent = intent
				manager.specialIntents[managerKey] = intent
			}
		} else {
			// We already have an oplog and this is a low priority oplog.
			if manager.oplogIntent.DB != "" {
				// If the manager's current oplog is also low priority, set a conflict.
				manager.oplogConflict = true
			}
			// No matter what, set this lower priority oplog to be a normal intent.
			manager.putNormalIntent(intent)
		}
	} else {
		if intent.DB == "" && intent.C == "oplog" {
			// If this is a normal oplog, then add it as an oplog intent.
			if existing := manager.specialIntents[managerKey]; existing != nil {
				existing.MergeIntent(intent)
				return
			}
			manager.oplogIntent = intent
			manager.specialIntents[managerKey] = intent
		} else {
			manager.putNormalIntent(intent)
		}
	}
}

func (manager *Manager) putNormalIntent(intent *Intent) {
	manager.putNormalIntentWithNamespace(intent.Namespace(), intent)
}

func (manager *Manager) putNormalIntentWithNamespace(ns string, intent *Intent) {
	// BSON and metadata files for the same collection are merged
	// into the same intent. This is done to allow for simple
	// pairing of BSON + metadata without keeping track of the
	// state of the filepath walker
	if existing := manager.intents[ns]; existing != nil {
		if existing.Namespace() != intent.Namespace() {
			// remove old destination, add new one
			dst := existing.Namespace()
			dsts := manager.destinations[dst]
			i := util.StringSliceIndex(dsts, ns)
			manager.destinations[dst] = append(dsts[:i], dsts[i+1:]...)

			dsts = manager.destinations[intent.Namespace()]
			manager.destinations[intent.Namespace()] = append(dsts, ns)
		}
		existing.MergeIntent(intent)
		return
	}

	// if key doesn't already exist, add it to the manager
	manager.intents[ns] = intent
	manager.intentsByDiscoveryOrder = append(manager.intentsByDiscoveryOrder, intent)

	manager.destinations[intent.Namespace()] = append(manager.destinations[intent.Namespace()], ns)
}

// Put inserts an intent into the manager with the same source namespace as
// its destinations.
func (manager *Manager) Put(intent *Intent) {
	log.Logvf(log.DebugLow, "enqueued collection '%v'", intent.Namespace())
	manager.PutWithNamespace(intent.Namespace(), intent)
}

// PutWithNamespace inserts an intent into the manager with the source set
// to the provided namespace. Intents for the same collection are merged
// together, so that BSON and metadata files for the same collection are
// returned in the same intent.
func (manager *Manager) PutWithNamespace(ns string, intent *Intent) {
	if intent == nil {
		panic("cannot insert nil *Intent into IntentManager")
	}
	db, _ := common.SplitNamespace(ns)

	// bucket special-case collections
	if intent.IsOplog() {
		manager.PutOplogIntent(intent, intent.Namespace())
		return
	}
	if intent.IsSystemIndexes() {
		if intent.BSONFile != nil {
			manager.indexIntents[db] = intent
			manager.specialIntents[ns] = intent
		}
		return
	}
	if intent.IsUsers() {
		if intent.BSONFile != nil {
			manager.usersIntent = intent
			manager.specialIntents[ns] = intent
		}
		return
	}
	if intent.IsRoles() {
		if intent.BSONFile != nil {
			manager.rolesIntent = intent
			manager.specialIntents[ns] = intent
		}
		return
	}
	if intent.IsAuthVersion() {
		if intent.BSONFile != nil {
			manager.versionIntent = intent
			manager.specialIntents[ns] = intent
		}
		return
	}

	manager.putNormalIntentWithNamespace(ns, intent)
}

func (manager *Manager) GetOplogConflict() bool {
	return manager.oplogConflict
}

func (manager *Manager) GetDestinationConflicts() (errs []DestinationConflictError) {
	for dst, srcs := range manager.destinations {
		if len(srcs) <= 1 {
			continue
		}
		for _, src := range srcs {
			errs = append(errs, DestinationConflictError{Dst: dst, Src: src})
		}
	}
	return
}

// Intents returns a slice containing all of the intents in the manager.
// Intents is not thread safe
func (manager *Manager) Intents() []*Intent {
	allIntents := []*Intent{}
	for _, intent := range manager.intents {
		allIntents = append(allIntents, intent)
	}
	for _, intent := range manager.indexIntents {
		allIntents = append(allIntents, intent)
	}
	if manager.oplogIntent != nil {
		allIntents = append(allIntents, manager.oplogIntent)
	}
	if manager.usersIntent != nil {
		allIntents = append(allIntents, manager.usersIntent)
	}
	if manager.rolesIntent != nil {
		allIntents = append(allIntents, manager.rolesIntent)
	}
	if manager.versionIntent != nil {
		allIntents = append(allIntents, manager.versionIntent)
	}
	return allIntents
}

func (manager *Manager) IntentForNamespace(ns string) *Intent {
	intent := manager.intents[ns]
	if intent != nil {
		return intent
	}
	intent = manager.specialIntents[ns]
	return intent
}

// Pop returns the next available intent from the manager. If the manager is
// empty, it returns nil. Pop is thread safe.
func (manager *Manager) Pop() *Intent {
	return manager.prioritizer.Get()
}

// Peek returns a copy of a stored intent from the manager without removing
// the intent. This method is useful for edge cases that need to look ahead
// at what collections are in the manager before they are scheduled.
//
// NOTE: There are no guarantees that peek will return a usable
// intent after Finalize() is called.
func (manager *Manager) Peek() *Intent {
	if len(manager.intentsByDiscoveryOrder) == 0 {
		return nil
	}
	intentCopy := *manager.intentsByDiscoveryOrder[0]
	return &intentCopy
}

// Finish tells the prioritizer that mongorestore is done restoring
// the given collection intent.
func (manager *Manager) Finish(intent *Intent) {
	manager.prioritizer.Finish(intent)
}

// Oplog returns the intent representing the oplog, which isn't
// stored with the other intents, because it is dumped and restored in
// a very different way from other collections.
func (manager *Manager) Oplog() *Intent {
	return manager.oplogIntent
}

// SystemIndexes returns the system.indexes bson for a database
func (manager *Manager) SystemIndexes(dbName string) *Intent {
	return manager.indexIntents[dbName]
}

// SystemIndexes returns the databases for which there are system.indexes
func (manager *Manager) SystemIndexDBs() []string {
	databases := []string{}
	for dbname := range manager.indexIntents {
		databases = append(databases, dbname)
	}
	return databases
}

// Users returns the intent of the users collection to restore, a special case
func (manager *Manager) Users() *Intent {
	return manager.usersIntent
}

// Roles returns the intent of the user roles collection to restore, a special case
func (manager *Manager) Roles() *Intent {
	return manager.rolesIntent
}

// AuthVersion returns the intent of the version collection to restore, a special case
func (manager *Manager) AuthVersion() *Intent {
	return manager.versionIntent
}

// Finalize processes the intents for prioritization. Currently only two
// kinds of prioritizers are supported. No more "Put" operations may be done
// after finalize is called.
func (manager *Manager) Finalize(pType PriorityType) {
	switch pType {
	case Legacy:
		log.Logv(log.DebugHigh, "finalizing intent manager with legacy prioritizer")
		manager.prioritizer = NewLegacyPrioritizer(manager.intentsByDiscoveryOrder)
	case LongestTaskFirst:
		log.Logv(log.DebugHigh, "finalizing intent manager with longest task first prioritizer")
		manager.prioritizer = NewLongestTaskFirstPrioritizer(manager.intentsByDiscoveryOrder)
	case MultiDatabaseLTF:
		log.Logv(log.DebugHigh, "finalizing intent manager with multi-database longest task first prioritizer")
		manager.prioritizer = NewMultiDatabaseLTFPrioritizer(manager.intentsByDiscoveryOrder)
	default:
		panic("cannot initialize IntentPrioritizer with unknown type")
	}
	// release these for the garbage collector and to ensure code correctness
	manager.intents = nil
	manager.intentsByDiscoveryOrder = nil
}

func (manager *Manager) UsePrioritizer(prioritizer IntentPrioritizer) {
	manager.prioritizer = prioritizer
}

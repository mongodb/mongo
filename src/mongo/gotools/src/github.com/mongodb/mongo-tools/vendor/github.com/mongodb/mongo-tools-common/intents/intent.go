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

	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/util"
	"go.mongodb.org/mongo-driver/bson"
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
	Options bson.M

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

func (it *Intent) IsSystemProfile() bool {
	return it.C == "system.profile"
}

func (it *Intent) IsSpecialCollection() bool {
	// can't see oplog as special collection because when restore from archive it need to be a RegularCollectionReceiver
	return it.IsSystemIndexes() || it.IsUsers() || it.IsRoles() || it.IsAuthVersion() || it.IsSystemProfile()
}

func (it *Intent) IsView() bool {
	if it.Options == nil {
		return false
	}
	_, isView := it.Options["viewOn"]
	return isView
}

func (it *Intent) MergeIntent(newIt *Intent) {
	// merge new intent into old intent
	if it.BSONFile == nil {
		it.BSONFile = newIt.BSONFile
	}
	if it.Size == 0 {
		it.Size = newIt.Size
	}
	if it.Location == "" {
		it.Location = newIt.Location
	}
	if it.MetadataFile == nil {
		it.MetadataFile = newIt.MetadataFile
	}
	if it.MetadataLocation == "" {
		it.MetadataLocation = newIt.MetadataLocation
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
func (mgr *Manager) PutOplogIntent(intent *Intent, managerKey string) {
	if mgr.smartPickOplog {
		if existing := mgr.specialIntents[managerKey]; existing != nil {
			existing.MergeIntent(intent)
			return
		}
		if mgr.oplogIntent == nil {
			// If there is no oplog intent, make this one the oplog.
			mgr.oplogIntent = intent
			mgr.specialIntents[managerKey] = intent
		} else if intent.DB == "" {
			// We already have an oplog and this is a top priority oplog.
			if mgr.oplogIntent.DB == "" {
				// If the manager's current oplog is also top priority, we have a
				// conflict and ignore this oplog.
				mgr.oplogConflict = true
			} else {
				// If the manager's current oplog is lower priority, replace it and
				// move that one to be a normal intent.
				mgr.putNormalIntent(mgr.oplogIntent)
				delete(mgr.specialIntents, mgr.oplogIntent.Namespace())
				mgr.oplogIntent = intent
				mgr.specialIntents[managerKey] = intent
			}
		} else {
			// We already have an oplog and this is a low priority oplog.
			if mgr.oplogIntent.DB != "" {
				// If the manager's current oplog is also low priority, set a conflict.
				mgr.oplogConflict = true
			}
			// No matter what, set this lower priority oplog to be a normal intent.
			mgr.putNormalIntent(intent)
		}
	} else {
		if intent.DB == "" && intent.C == "oplog" {
			// If this is a normal oplog, then add it as an oplog intent.
			if existing := mgr.specialIntents[managerKey]; existing != nil {
				existing.MergeIntent(intent)
				return
			}
			mgr.oplogIntent = intent
			mgr.specialIntents[managerKey] = intent
		} else {
			mgr.putNormalIntent(intent)
		}
	}
}

func (mgr *Manager) putNormalIntent(intent *Intent) {
	mgr.putNormalIntentWithNamespace(intent.Namespace(), intent)
}

func (mgr *Manager) putNormalIntentWithNamespace(ns string, intent *Intent) {
	// BSON and metadata files for the same collection are merged
	// into the same intent. This is done to allow for simple
	// pairing of BSON + metadata without keeping track of the
	// state of the filepath walker
	if existing := mgr.intents[ns]; existing != nil {
		if existing.Namespace() != intent.Namespace() {
			// remove old destination, add new one
			dst := existing.Namespace()
			dsts := mgr.destinations[dst]
			i := util.StringSliceIndex(dsts, ns)
			mgr.destinations[dst] = append(dsts[:i], dsts[i+1:]...)

			dsts = mgr.destinations[intent.Namespace()]
			mgr.destinations[intent.Namespace()] = append(dsts, ns)
		}
		existing.MergeIntent(intent)
		return
	}

	// if key doesn't already exist, add it to the manager
	mgr.intents[ns] = intent
	mgr.intentsByDiscoveryOrder = append(mgr.intentsByDiscoveryOrder, intent)

	mgr.destinations[intent.Namespace()] = append(mgr.destinations[intent.Namespace()], ns)
}

// Put inserts an intent into the manager with the same source namespace as
// its destinations.
func (mgr *Manager) Put(intent *Intent) {
	log.Logvf(log.DebugLow, "enqueued collection '%v'", intent.Namespace())
	mgr.PutWithNamespace(intent.Namespace(), intent)
}

// PutWithNamespace inserts an intent into the manager with the source set
// to the provided namespace. Intents for the same collection are merged
// together, so that BSON and metadata files for the same collection are
// returned in the same intent.
func (mgr *Manager) PutWithNamespace(ns string, intent *Intent) {
	if intent == nil {
		panic("cannot insert nil *Intent into IntentManager")
	}
	db, _ := util.SplitNamespace(ns)

	// bucket special-case collections
	if intent.IsOplog() {
		mgr.PutOplogIntent(intent, intent.Namespace())
		return
	}
	if intent.IsSystemIndexes() {
		if intent.BSONFile != nil {
			mgr.indexIntents[db] = intent
			mgr.specialIntents[ns] = intent
		}
		return
	}
	if intent.IsUsers() {
		if intent.BSONFile != nil {
			mgr.usersIntent = intent
			mgr.specialIntents[ns] = intent
		}
		return
	}
	if intent.IsRoles() {
		if intent.BSONFile != nil {
			mgr.rolesIntent = intent
			mgr.specialIntents[ns] = intent
		}
		return
	}
	if intent.IsAuthVersion() {
		if intent.BSONFile != nil {
			mgr.versionIntent = intent
			mgr.specialIntents[ns] = intent
		}
		return
	}

	mgr.putNormalIntentWithNamespace(ns, intent)
}

func (mgr *Manager) GetOplogConflict() bool {
	return mgr.oplogConflict
}

func (mgr *Manager) GetDestinationConflicts() (errs []DestinationConflictError) {
	for dst, srcs := range mgr.destinations {
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
func (mgr *Manager) Intents() []*Intent {
	allIntents := []*Intent{}
	for _, intent := range mgr.intents {
		allIntents = append(allIntents, intent)
	}
	for _, intent := range mgr.indexIntents {
		allIntents = append(allIntents, intent)
	}
	if mgr.oplogIntent != nil {
		allIntents = append(allIntents, mgr.oplogIntent)
	}
	if mgr.usersIntent != nil {
		allIntents = append(allIntents, mgr.usersIntent)
	}
	if mgr.rolesIntent != nil {
		allIntents = append(allIntents, mgr.rolesIntent)
	}
	if mgr.versionIntent != nil {
		allIntents = append(allIntents, mgr.versionIntent)
	}
	return allIntents
}

func (mgr *Manager) IntentForNamespace(ns string) *Intent {
	intent := mgr.intents[ns]
	if intent != nil {
		return intent
	}
	intent = mgr.specialIntents[ns]
	return intent
}

// Pop returns the next available intent from the manager. If the manager is
// empty, it returns nil. Pop is thread safe.
func (mgr *Manager) Pop() *Intent {
	return mgr.prioritizer.Get()
}

// Peek returns a copy of a stored intent from the manager without removing
// the intent. This method is useful for edge cases that need to look ahead
// at what collections are in the manager before they are scheduled.
//
// NOTE: There are no guarantees that peek will return a usable
// intent after Finalize() is called.
func (mgr *Manager) Peek() *Intent {
	if len(mgr.intentsByDiscoveryOrder) == 0 {
		return nil
	}
	intentCopy := *mgr.intentsByDiscoveryOrder[0]
	return &intentCopy
}

// Finish tells the prioritizer that mongorestore is done restoring
// the given collection intent.
func (mgr *Manager) Finish(intent *Intent) {
	mgr.prioritizer.Finish(intent)
}

// Oplog returns the intent representing the oplog, which isn't
// stored with the other intents, because it is dumped and restored in
// a very different way from other collections.
func (mgr *Manager) Oplog() *Intent {
	return mgr.oplogIntent
}

// SystemIndexes returns the system.indexes bson for a database
func (mgr *Manager) SystemIndexes(dbName string) *Intent {
	return mgr.indexIntents[dbName]
}

// SystemIndexes returns the databases for which there are system.indexes
func (mgr *Manager) SystemIndexDBs() []string {
	databases := []string{}
	for dbname := range mgr.indexIntents {
		databases = append(databases, dbname)
	}
	return databases
}

// Users returns the intent of the users collection to restore, a special case
func (mgr *Manager) Users() *Intent {
	return mgr.usersIntent
}

// Roles returns the intent of the user roles collection to restore, a special case
func (mgr *Manager) Roles() *Intent {
	return mgr.rolesIntent
}

// AuthVersion returns the intent of the version collection to restore, a special case
func (mgr *Manager) AuthVersion() *Intent {
	return mgr.versionIntent
}

// Finalize processes the intents for prioritization. Currently only two
// kinds of prioritizers are supported. No more "Put" operations may be done
// after finalize is called.
func (mgr *Manager) Finalize(pType PriorityType) {
	switch pType {
	case Legacy:
		log.Logv(log.DebugHigh, "finalizing intent manager with legacy prioritizer")
		mgr.prioritizer = newLegacyPrioritizer(mgr.intentsByDiscoveryOrder)
	case LongestTaskFirst:
		log.Logv(log.DebugHigh, "finalizing intent manager with longest task first prioritizer")
		mgr.prioritizer = newLongestTaskFirstPrioritizer(mgr.intentsByDiscoveryOrder)
	case MultiDatabaseLTF:
		log.Logv(log.DebugHigh, "finalizing intent manager with multi-database longest task first prioritizer")
		mgr.prioritizer = newMultiDatabaseLTFPrioritizer(mgr.intentsByDiscoveryOrder)
	default:
		panic("cannot initialize IntentPrioritizer with unknown type")
	}
	// release these for the garbage collector and to ensure code correctness
	mgr.intents = nil
	mgr.intentsByDiscoveryOrder = nil
}

func (mgr *Manager) UsePrioritizer(prioritizer IntentPrioritizer) {
	mgr.prioritizer = prioritizer
}

// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package archive

import (
	"bytes"
	"fmt"
	"io"
	"path/filepath"
	"sync/atomic"

	"github.com/mongodb/mongo-tools/common"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"gopkg.in/mgo.v2/bson"
)

//MetadataFile implements intents.file
type MetadataFile struct {
	pos int64 // updated atomically, aligned at the beginning of the struct
	*bytes.Buffer
	Intent *intents.Intent
}

func (md *MetadataFile) Open() error {
	return nil
}
func (md *MetadataFile) Close() error {
	return nil
}

func (md *MetadataFile) Read(p []byte) (int, error) {
	n, err := md.Buffer.Read(p)
	atomic.AddInt64(&md.pos, int64(n))
	return n, err
}

func (md *MetadataFile) Pos() int64 {
	return atomic.LoadInt64(&md.pos)
}

// DirLike represents the group of methods done on directories and files in dump directories,
// or in archives, when mongorestore is figuring out what intents to create.
type DirLike interface {
	Name() string
	Path() string
	Size() int64
	IsDir() bool
	Stat() (DirLike, error)
	ReadDir() ([]DirLike, error)
	Parent() DirLike
}

// Prelude represents the knowledge gleaned from reading the prelude out of the archive.
type Prelude struct {
	Header                 *Header
	DBS                    []string
	NamespaceMetadatas     []*CollectionMetadata
	NamespaceMetadatasByDB map[string][]*CollectionMetadata
}

// Read consumes and checks the magic number at the beginning of the archive,
// then it runs the parser with a Prelude as its consumer.
func (prelude *Prelude) Read(in io.Reader) error {
	readMagicNumberBuf := make([]byte, 4)
	_, err := io.ReadAtLeast(in, readMagicNumberBuf, 4)
	if err != nil {
		return fmt.Errorf("I/O failure reading beginning of archive: %v", err)
	}
	readMagicNumber := uint32(
		(uint32(readMagicNumberBuf[0]) << 0) |
			(uint32(readMagicNumberBuf[1]) << 8) |
			(uint32(readMagicNumberBuf[2]) << 16) |
			(uint32(readMagicNumberBuf[3]) << 24),
	)

	if readMagicNumber != MagicNumber {
		return fmt.Errorf("stream or file does not appear to be a mongodump archive")
	}

	if prelude.NamespaceMetadatasByDB != nil {
		prelude.NamespaceMetadatasByDB = make(map[string][]*CollectionMetadata, 0)
	}

	parser := Parser{In: in}
	parserConsumer := &preludeParserConsumer{prelude: prelude}
	return parser.ReadBlock(parserConsumer)
}

// NewPrelude generates a Prelude using the contents of an intent.Manager.
func NewPrelude(manager *intents.Manager, concurrentColls int, serverVersion string) (*Prelude, error) {
	prelude := Prelude{
		Header: &Header{
			FormatVersion:         archiveFormatVersion,
			ServerVersion:         serverVersion,
			ToolVersion:           options.VersionStr,
			ConcurrentCollections: int32(concurrentColls),
		},
		NamespaceMetadatasByDB: make(map[string][]*CollectionMetadata, 0),
	}
	allIntents := manager.Intents()
	for _, intent := range allIntents {
		if intent.MetadataFile != nil {
			archiveMetadata, ok := intent.MetadataFile.(*MetadataFile)
			if !ok {
				return nil, fmt.Errorf("MetadataFile is not an archive.Metadata")
			}
			prelude.AddMetadata(&CollectionMetadata{
				Database:   intent.DB,
				Collection: intent.C,
				Metadata:   archiveMetadata.Buffer.String(),
			})
		} else {
			prelude.AddMetadata(&CollectionMetadata{
				Database:   intent.DB,
				Collection: intent.C,
			})
		}
	}
	return &prelude, nil
}

// AddMetadata adds a metadata data structure to a prelude and does the required bookkeeping.
func (prelude *Prelude) AddMetadata(cm *CollectionMetadata) {
	prelude.NamespaceMetadatas = append(prelude.NamespaceMetadatas, cm)
	if prelude.NamespaceMetadatasByDB == nil {
		prelude.NamespaceMetadatasByDB = make(map[string][]*CollectionMetadata)
	}
	_, ok := prelude.NamespaceMetadatasByDB[cm.Database]
	if !ok {
		prelude.DBS = append(prelude.DBS, cm.Database)
	}
	prelude.NamespaceMetadatasByDB[cm.Database] = append(prelude.NamespaceMetadatasByDB[cm.Database], cm)
	log.Logvf(log.Info, "archive prelude %v.%v", cm.Database, cm.Collection)
}

// Write writes the archive header.
func (prelude *Prelude) Write(out io.Writer) error {
	magicNumberBytes := make([]byte, 4)
	for i := range magicNumberBytes {
		magicNumberBytes[i] = byte(uint32(MagicNumber) >> uint(i*8))
	}
	_, err := out.Write(magicNumberBytes)
	if err != nil {
		return err
	}
	buf, err := bson.Marshal(prelude.Header)
	if err != nil {
		return err
	}
	_, err = out.Write(buf)
	if err != nil {
		return err
	}
	for _, cm := range prelude.NamespaceMetadatas {
		buf, err = bson.Marshal(cm)
		if err != nil {
			return err
		}
		_, err = out.Write(buf)
		if err != nil {
			return err
		}
	}
	_, err = out.Write(terminatorBytes)
	if err != nil {
		return err
	}
	return nil
}

// preludeParserConsumer wraps a Prelude, and implements ParserConsumer.
type preludeParserConsumer struct {
	prelude *Prelude
}

// HeaderBSON is part of the ParserConsumer interface, it unmarshals archive Headers.
func (hpc *preludeParserConsumer) HeaderBSON(data []byte) error {
	hpc.prelude.Header = &Header{}
	err := bson.Unmarshal(data, hpc.prelude.Header)
	if err != nil {
		return err
	}
	return nil
}

// BodyBSON is part of the ParserConsumer interface, it unmarshals CollectionMetadata's.
func (hpc *preludeParserConsumer) BodyBSON(data []byte) error {
	cm := &CollectionMetadata{}
	err := bson.Unmarshal(data, cm)
	if err != nil {
		return err
	}
	hpc.prelude.AddMetadata(cm)
	return nil
}

// BodyBSON is part of the ParserConsumer interface.
func (hpc *preludeParserConsumer) End() error {
	return nil
}

// PreludeExplorer implements DirLike. PreludeExplorer represent the databases, collections,
// and their metadata json files, of an archive, in such a way that they can be explored like a filesystem.
type PreludeExplorer struct {
	prelude    *Prelude
	database   string
	collection string
	isMetadata bool
}

// NewPreludeExplorer creates a PreludeExplorer from a Prelude.
func (prelude *Prelude) NewPreludeExplorer() (*PreludeExplorer, error) {
	pe := &PreludeExplorer{
		prelude: prelude,
	}
	return pe, nil
}

// Name is part of the DirLike interface. It synthesizes a filename for the given "location" the prelude.
func (pe *PreludeExplorer) Name() string {
	if pe.collection == "" {
		return pe.database
	}
	if pe.isMetadata {
		return pe.collection + ".metadata.json"
	}
	return pe.collection + ".bson"
}

// Path is part of the DirLike interface. It creates the full path for the "location" in the prelude.
func (pe *PreludeExplorer) Path() string {
	if pe.collection == "" {
		return pe.database
	}
	if pe.database == "" {
		return pe.Name()
	}
	return filepath.Join(pe.database, pe.Name())
}

// Size is part of the DirLike interface. It returns the size from the metadata
// of the prelude, if the "location" is a collection.
func (pe *PreludeExplorer) Size() int64 {
	if pe.IsDir() {
		return 0
	}
	for _, ns := range pe.prelude.NamespaceMetadatas {
		if ns.Database == pe.database && ns.Collection == pe.collection {
			return int64(ns.Size)
		}
	}
	return 0
}

// IsDir is part of the DirLike interface. All pes that are not collections are Dirs.
func (pe *PreludeExplorer) IsDir() bool {
	return pe.collection == ""
}

// Stat is part of the DirLike interface. os.Stat returns a FileInfo, and since
// DirLike is similar to FileInfo, we just return the pe, here.
func (pe *PreludeExplorer) Stat() (DirLike, error) {
	return pe, nil
}

// ReadDir is part of the DirLIke interface. ReadDir generates a list of PreludeExplorers
// whose "locations" are encapsulated by the current pes "location".
//
//  "dump/oplog.bson"     => &PreludeExplorer{ database: "", collection: "oplog.bson" }
//  "dump/test/"          => &PreludeExplorer{ database: "test", collection: "foo.bson" }
//  "dump/test/foo.bson"  => &PreludeExplorer{ database: "test", collection: "" }
//  "dump/test/foo.json"  => &PreludeExplorer{ database: "test", collection: "foo", isMetadata: true }
//
func (pe *PreludeExplorer) ReadDir() ([]DirLike, error) {
	if !pe.IsDir() {
		return nil, fmt.Errorf("not a directory")
	}
	pes := []DirLike{}
	if pe.database == "" {
		// when reading the top level of the archive, we need return all of the
		// collections that are not bound to a database, aka, the oplog, and then all of
		// the databases the prelude stores all top-level collections as collections in
		// the "" database
		topLevelNamespaceMetadatas, ok := pe.prelude.NamespaceMetadatasByDB[""]
		if ok {
			for _, topLevelNamespaceMetadata := range topLevelNamespaceMetadatas {
				pes = append(pes, &PreludeExplorer{
					prelude:    pe.prelude,
					collection: topLevelNamespaceMetadata.Collection,
				})
				if topLevelNamespaceMetadata.Metadata != "" {
					pes = append(pes, &PreludeExplorer{
						prelude:    pe.prelude,
						collection: topLevelNamespaceMetadata.Collection,
						isMetadata: true,
					})
				}
			}
		}
		for _, db := range pe.prelude.DBS {
			pes = append(pes, &PreludeExplorer{
				prelude:  pe.prelude,
				database: db,
			})
		}
	} else {
		// when reading the contents of a database directory, we just return all of the bson and
		// json files for all of the collections bound to that database
		namespaceMetadatas, ok := pe.prelude.NamespaceMetadatasByDB[pe.database]
		if !ok {
			return nil, fmt.Errorf("no such directory") //TODO: replace with real ERRNOs?
		}
		for _, namespaceMetadata := range namespaceMetadatas {
			pes = append(pes, &PreludeExplorer{
				prelude:    pe.prelude,
				database:   pe.database,
				collection: namespaceMetadata.Collection,
			})
			if namespaceMetadata.Metadata != "" {
				pes = append(pes, &PreludeExplorer{
					prelude:    pe.prelude,
					database:   pe.database,
					collection: namespaceMetadata.Collection,
					isMetadata: true,
				})
			}
		}
	}
	return pes, nil
}

// Parent is part of the DirLike interface. It returns a pe without a collection, if there is one,
// otherwise, without a database.
func (pe *PreludeExplorer) Parent() DirLike {
	if pe.collection != "" {
		return &PreludeExplorer{
			prelude:  pe.prelude,
			database: pe.database,
		}
	}
	return &PreludeExplorer{
		prelude: pe.prelude,
	}
}

// MetadataPreludeFile is part of the intents.file. It allows the metadata contained in the prelude to be opened and read
type MetadataPreludeFile struct {
	pos     int64 // updated atomically, aligned at the beginning of the struct
	Intent  *intents.Intent
	Origin  string
	Prelude *Prelude
	*bytes.Buffer
}

// Open is part of the intents.file interface, it finds the metadata in the prelude and creates a bytes.Buffer from it.
func (mpf *MetadataPreludeFile) Open() error {
	db, c := common.SplitNamespace(mpf.Origin)
	dbMetadatas, ok := mpf.Prelude.NamespaceMetadatasByDB[db]
	if !ok {
		return fmt.Errorf("no metadata found for '%s'", db)
	}
	for _, metadata := range dbMetadatas {
		if metadata.Collection == c {
			mpf.Buffer = bytes.NewBufferString(metadata.Metadata)
			return nil
		}
	}
	return fmt.Errorf("no matching metadata found for '%s'", mpf.Origin)
}

// Close is part of the intents.file interface.
func (mpf *MetadataPreludeFile) Close() error {
	mpf.Buffer = nil
	return nil
}

func (mpf *MetadataPreludeFile) Read(p []byte) (int, error) {
	n, err := mpf.Buffer.Read(p)
	atomic.AddInt64(&mpf.pos, int64(n))
	return n, err
}

func (mpf *MetadataPreludeFile) Pos() int64 {
	return atomic.LoadInt64(&mpf.pos)
}

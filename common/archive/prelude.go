package archive

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/intents"
	"gopkg.in/mgo.v2/bson"
	"io"
)

type ArchivePrelude struct {
	Header                  *ArchiveHeader
	CollectionMetadatas     []*CollectionMetadata
	DBS                     []string
	CollectionMetadatasByDB map[string][]*CollectionMetadata
}

func (prelude *ArchivePrelude) Read(in io.Reader) error {
	magicNumberBuf := make([]byte, 4)
	_, err := io.ReadAtLeast(in, magicNumberBuf, 4)
	if err != nil {
		return err
	}
	magicNumber := int32(
		(uint32(magicNumberBuf[0]) << 0) |
			(uint32(magicNumberBuf[1]) << 8) |
			(uint32(magicNumberBuf[2]) << 16) |
			(uint32(magicNumberBuf[3]) << 24),
	)

	if magicNumber != MagicNumber {
		return fmt.Errorf("stream or file does not apear to be a mongodump archive")
	}

	if prelude.CollectionMetadatasByDB != nil {
		prelude.CollectionMetadatasByDB = make(map[string][]*CollectionMetadata, 0)
	}

	parser := Parser{In: in}
	parserConsumer := &preludeParserConsumer{prelude: prelude}
	err = parser.ReadBlock(parserConsumer)
	if err != nil {
		return err
	}
	return nil
}

func NewPrelude(manager *intents.Manager, maxProcs int) (*ArchivePrelude, error) {
	prelude := ArchivePrelude{
		Header: &ArchiveHeader{
			ArchiveFormatVersion:  archiveFormatVersion,
			ConcurrentCollections: int32(maxProcs),
		},
		CollectionMetadatasByDB: make(map[string][]*CollectionMetadata, 0),
	}
	allIntents := manager.Intents()
	for _, intent := range allIntents {
		if intent.MetadataFile != nil {
			archiveMetadata, ok := intent.MetadataFile.(*Metadata)
			if !ok {
				return nil, fmt.Errorf("MetadataFile is not an ArchiveMetadata")
			}
			prelude.AddMetadata(&CollectionMetadata{
				Database:   intent.DB,
				Collection: intent.C,
				Metadata:   archiveMetadata.Buffer.String(),
			})
		}
	}
	return &prelude, nil
}

func (prelude *ArchivePrelude) AddMetadata(cm *CollectionMetadata) {
	prelude.CollectionMetadatas = append(prelude.CollectionMetadatas, cm)
	if prelude.CollectionMetadatasByDB == nil {
		prelude.CollectionMetadatasByDB = make(map[string][]*CollectionMetadata)
	}
	_, ok := prelude.CollectionMetadatasByDB[cm.Database]
	if !ok {
		prelude.DBS = append(prelude.DBS, cm.Database)
	}
	prelude.CollectionMetadatasByDB[cm.Database] = append(prelude.CollectionMetadatasByDB[cm.Database], cm)
}

type preludeParserConsumer struct {
	prelude *ArchivePrelude
}

func (hpc *preludeParserConsumer) HeaderBSON(data []byte) error {
	hpc.prelude.Header = &ArchiveHeader{}
	err := bson.Unmarshal(data, hpc.prelude.Header)
	if err != nil {
		return err
	}
	return nil
}
func (hpc *preludeParserConsumer) BodyBSON(data []byte) error {
	cm := &CollectionMetadata{}
	err := bson.Unmarshal(data, cm)
	if err != nil {
		return err
	}
	hpc.prelude.AddMetadata(cm)
	return nil
}

func (hpc *preludeParserConsumer) End() error {
	return nil
}

func (prelude *ArchivePrelude) Write(out io.Writer) error {
	magicNumberBytes := make([]byte, 4)
	for i, _ := range magicNumberBytes {
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
	for _, cm := range prelude.CollectionMetadatas {
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

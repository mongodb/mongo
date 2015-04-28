package archive

import (
	"io"
)

// NamespaceHeader is a datastructure that, as bson, is found in archives where it indicates
// that either the subsequent streem of bson belongs to this new namespace, or that the
// indicated namespace will have no nore documents (EOF)
type NamespaceHeader struct {
	Database   string `bson:"db"`
	Collection string `bson:"collection"`
	EOF        bool   `bson:"EOF",omitempty`
}

// CollectionMetadata is a datastructure that, as bson, is found the the prelude of the archive.
// There is one CollectionMetadata per collection that will be in the archive.
type CollectionMetadata struct {
	Database   string `bson:"db"`
	Collection string `bson:"collection"`
	Metadata   string `bson:"metadata"`
	Size       int    `bson:"size"`
}

// Header is a datastructure that, as bson, is found immediately after the magic
// numbner in the archive, before any CollectionMetadata's
type Header struct {
	ConcurrentCollections int32  `bson:"concurrent_collections",omitempty`
	ArchiveFormatVersion  string `bson:"version"`
}

const minBSONSize = 4 + 1 // an empty bson document should be exactly five bytes long

var terminator int32 = -1
var terminatorBytes = []byte{0xFF, 0xFF, 0xFF, 0xFF} // TODO, rectify this with terminator

// MagicNumber is four byte that are found at the begining of the archive that indicate that
// the byte stream is an archive, as opposed to anything else, including a stream of bson documents
const MagicNumber int32 = 0x6de9818b
const archiveFormatVersion = "0.1"

// Writer is the top level object to contain information about archives in mongodump
type Writer struct {
	Out     io.WriteCloser
	Prelude *Prelude
	Mux     *Multiplexer
}

// Reader is the top level object to contain information about archives in mongorestore
type Reader struct {
	In      io.ReadCloser
	Demux   *Demultiplexer
	Prelude *Prelude
}

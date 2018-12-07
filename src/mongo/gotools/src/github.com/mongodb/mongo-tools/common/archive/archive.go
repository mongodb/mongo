package archive

import "io"

// NamespaceHeader is a data structure that, as BSON, is found in archives where it indicates
// that either the subsequent stream of BSON belongs to this new namespace, or that the
// indicated namespace will have no more documents (EOF)
type NamespaceHeader struct {
	Database   string `bson:"db"`
	Collection string `bson:"collection"`
	EOF        bool   `bson:"EOF"`
	CRC        int64  `bson:"CRC"`
}

// CollectionMetadata is a data structure that, as BSON, is found in the prelude of the archive.
// There is one CollectionMetadata per collection that will be in the archive.
type CollectionMetadata struct {
	Database   string `bson:"db"`
	Collection string `bson:"collection"`
	Metadata   string `bson:"metadata"`
	Size       int    `bson:"size"`
}

// Header is a data structure that, as BSON, is found immediately after the magic
// number in the archive, before any CollectionMetadatas. It is the home of any archive level information
type Header struct {
	ConcurrentCollections int32  `bson:"concurrent_collections"`
	FormatVersion         string `bson:"version"`
	ServerVersion         string `bson:"server_version"`
	ToolVersion           string `bson:"tool_version"`
}

const minBSONSize = 4 + 1 // an empty BSON document should be exactly five bytes long

var terminator int32 = -1
var terminatorBytes = []byte{0xFF, 0xFF, 0xFF, 0xFF} // TODO, rectify this with terminator

// MagicNumber is four bytes that are found at the beginning of the archive that indicate that
// the byte stream is an archive, as opposed to anything else, including a stream of BSON documents
const MagicNumber uint32 = 0x8199e26d
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

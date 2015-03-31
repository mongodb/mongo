package archive

type CollectionHeader struct {
	Database   string `bson:"db"`
	Collection string `bson:"collection"`
	EOF        bool   `bson:"EOF",omitempty`
}
type CollectionMetadata struct {
	Database   string `bson:"db"`
	Collection string `bson:"collection"`
	Metadata   string `bson:"metadata"`
}

const minBSONSize = 4 + 1 // an empty bson document should be exactly five bytes long
const terminator int32 = -1

var terminatorBytes []byte = []byte{0xFF, 0xFF, 0xFF, 0xFF} // TODO, rectify this with terminator

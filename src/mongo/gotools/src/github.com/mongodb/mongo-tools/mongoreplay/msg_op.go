package mongoreplay

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"time"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// MsgOp is a struct for parsing OP_MSG as defined here:
// https://github.com/mongodb/mongo/blob/master/src/mongo/rpc/command_request.h.
type MsgOp struct {
	Header MsgHeader
	mgo.MsgOp

	CommandName string
	Database    string
}

// MsgOpReply is a struct representing the case of an OP_MSG which was a response
// from the server. It implements the Replyable interface and has fields for
// caching the docs found and cursor so that multiple calls to these methods
// do not incur the overhead of searching the underlying bson.
type MsgOpReply struct {
	MsgOp
	Latency  time.Duration
	cursorID *int64
	Docs     []bson.Raw
}

// MsgOpGetMore is a struct representing the case of an OP_MSG which was a getmore
// command. It implements the cursorsRewriteable interface and has a field for
// caching the cursor found so that multiple calls to these methods
// do not incur the overhead of searching the underlying bson.
type MsgOpGetMore struct {
	MsgOp
	cachedCursor *int64
}

// Abbreviated returns a serialization of the MsgOp, abbreviated.
func (msgOp *MsgOp) Abbreviated(chars int) string {
	body, err := msgOp.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("MsgOp sections:%v", Abbreviate(body, chars))
}

// OpCode returns the OpCode for the MsgOp.
func (msgOp *MsgOp) OpCode() OpCode {
	return OpCodeMessage
}

func (msgOp *MsgOp) getOpBodyString() (string, error) {
	var buffer bytes.Buffer
	for _, section := range msgOp.Sections {
		switch section.PayloadType {
		case mgo.MsgPayload0:
			dataAsJson, err := ConvertBSONValueToJSON(section.Data)
			if err != nil {
				return "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", msgOp, err)
			}
			asJSON, err := json.Marshal(dataAsJson)
			if err != nil {
				return "", fmt.Errorf("json marshal err: %#v - %v", msgOp, err)
			}
			buffer.WriteString(string(asJSON))

		case mgo.MsgPayload1:
			asPayloadType1, ok := section.Data.(mgo.PayloadType1)
			if !ok {
				return "", fmt.Errorf("incorrect type: expecting payload type 1")
			}
			buffer.WriteString("identifier: ")
			buffer.WriteString(asPayloadType1.Identifier)
			dataAsJson, err := ConvertBSONValueToJSON(asPayloadType1.Docs)
			if err != nil {
				return "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", msgOp, err)
			}
			asJSON, err := json.Marshal(dataAsJson)
			if err != nil {
				return "", fmt.Errorf("json marshal err: %#v - %v", msgOp, err)
			}
			buffer.WriteString("data: ")
			buffer.WriteString(string(asJSON))
		}
		buffer.WriteString(",")
	}
	return buffer.String(), nil
}

// FromReader extracts data from a serialized MsgOp into its concrete
// structure.
func (op *MsgOp) FromReader(r io.Reader) error {
	buf := [4]byte{}
	_, err := io.ReadFull(r, buf[:])
	if err != nil {
		return err
	}

	var checksumLength int
	op.Flags = uint32(getInt32(buf[:], 0))
	var checksumPresent bool
	if (op.Flags & mgo.MsgFlagChecksumPresent) != 0 {
		checksumPresent = true
		checksumLength = 4
	}
	offset := 4
	for int(op.Header.MessageLength)-(offset+checksumLength+MsgHeaderLen) > 0 {
		section, length, err := readSection(r)
		if err != nil {
			return err
		}
		op.Sections = append(op.Sections, section)
		offset += length
	}
	if checksumPresent {
		_, err := io.ReadFull(r, buf[:])
		if err != nil {
			return err
		}
		op.Checksum = uint32(getInt32(buf[:], 0))
	}
	_, err = op.getCommandName()
	if err != nil {
		return err
	}
	_, err = op.getDB()
	if err != nil {
		return err
	}
	return nil
}

// Execute performs the MsgOp on a given session, yielding the reply when
// successful (and an error otherwise).
func (op *MsgOp) Execute(socket *mgo.MongoSocket) (Replyable, error) {
	before := time.Now()
	_, sectionsData, _, resultReply, err := mgo.ExecOpWithReply(socket, &op.MsgOp)
	after := time.Now()
	if err != nil {
		return nil, err
	}
	replyAsMsgOp, ok := resultReply.(*mgo.MsgOp)
	if !ok {
		panic("reply from execution was not the correct type")
	}
	msgOp := MsgOp{
		MsgOp: *replyAsMsgOp,
	}

	resultDocs := []bson.Raw{}

	reader := bytes.NewReader(sectionsData)
	offset := 0
	for len(sectionsData)-offset > 0 {
		section, length, err := readSection(reader)
		if err != nil {
			return nil, err
		}
		offset += length
		msgOp.Sections = append(msgOp.Sections, section)
		docs, err := getCursorDocsFromMsgSection(section)

		if err != nil {
			return nil, err
		}
		resultDocs = append(resultDocs, docs...)
	}

	return &MsgOpReply{
		MsgOp:   msgOp,
		Latency: after.Sub(before),
		Docs:    resultDocs,
	}, nil
}

func (msgOp *MsgOpReply) getCursorID() (int64, error) {
	if msgOp.cursorID != nil {
		return *msgOp.cursorID, nil
	}
	if len(msgOp.Sections) == 0 {
		return 0, nil
	}

	payload0DataRaw, _, err := fetchPayload0Data(msgOp.Sections)
	if err != nil {
		return 0, err
	}
	id, err := getCursorID(payload0DataRaw)
	if err != nil {
		return 0, err
	}
	msgOp.cursorID = &id
	return *msgOp.cursorID, nil
}

// Meta returns metadata about the operation, useful for analysis of traffic.
func (msgOp *MsgOp) Meta() OpMetadata {
	return OpMetadata{"op_msg",
		msgOp.Database,
		msgOp.CommandName,
		map[string]interface{}{
			"sections": msgOp.Sections,
		},
	}
}

func (msgOp *MsgOpReply) getLatencyMicros() int64 {
	return int64(msgOp.Latency / (time.Microsecond))
}
func (msgOp *MsgOpReply) getNumReturned() int {
	return len(msgOp.Docs)
}

// getErrors fetches the DB errors present in the message data
func (msgOp *MsgOpReply) getErrors() []error {
	if len(msgOp.Sections) == 0 {
		return nil
	}

	payload0DataRaw, _, err := fetchPayload0Data(msgOp.Sections)
	if err != nil {
		return nil
	}

	doc := bson.D{}
	err = payload0DataRaw.Unmarshal(&doc)
	if err != nil {
		panic("failed to unmarshal Raw into bson.D")
	}
	return extractErrorsFromDoc(&doc)
}

// getCursorIDs returns the cursorIDs present in the getmore command
func (msgOpGM *MsgOpGetMore) getCursorIDs() ([]int64, error) {
	if msgOpGM.cachedCursor != nil {
		return []int64{*msgOpGM.cachedCursor}, nil
	}

	// there are no docs, so there is nothing to give
	if len(msgOpGM.Sections) == 0 {
		return nil, nil
	}

	payload0DataRaw, _, err := fetchPayload0Data(msgOpGM.Sections)
	if err != nil {
		return nil, err
	}

	cursorID, err := getGetMoreCursorID(payload0DataRaw)
	if err != nil {
		return nil, err
	}

	msgOpGM.cachedCursor = &cursorID
	return []int64{*msgOpGM.cachedCursor}, err
}

func (msgOpGM *MsgOpGetMore) setCursorIDs(newCursorIDs []int64) error {
	// there are no docs, so there is nothing to give
	if len(msgOpGM.Sections) == 0 {
		return nil
	}

	payload0DataRaw, sectionIx, err := fetchPayload0Data(msgOpGM.Sections)
	if err != nil {
		return err
	}

	newDoc, newCursorID, err := setCursorID(payload0DataRaw, newCursorIDs)

	msgOpGM.cachedCursor = &newCursorID
	newDocAsRaw := bson.Raw{}

	// transform the document from a bson.D to a bson.Raw
	newDocAsSlice, err := bson.Marshal(&newDoc)
	if err != nil {
		return err
	}
	err = bson.Unmarshal(newDocAsSlice, &newDocAsRaw)
	if err != nil {
		return err
	}
	msgOpGM.Sections[sectionIx].Data = &newDocAsRaw

	return nil
}

func readSection(r io.Reader) (mgo.MsgSection, int, error) {
	// Fetch payload type
	section := mgo.MsgSection{}
	offset := 0
	buf := [4]byte{}
	_, err := io.ReadFull(r, buf[:1])
	if err != nil {
		return mgo.MsgSection{}, 0, err
	}
	offset++

	// 2 cases
	// Case 1: Either we have a payload that just contains a bson document (payload == 0)
	// Case 2: We have a payload that contains a size, identifier, and a document (payload == 1)
	//    int32      size;
	//    cstring    identifier;
	//    document*  documents;

	switch buf[0] {

	// Case 1: payload == 0
	case mgo.MsgPayload0:
		section.PayloadType = mgo.MsgPayload0
		docAsSlice, err := ReadDocument(r)
		if err != nil {
			return mgo.MsgSection{}, 0, err
		}
		doc := &bson.Raw{}
		err = bson.Unmarshal(docAsSlice, doc)
		if err != nil {
			return mgo.MsgSection{}, 0, err
		}
		section.Data = doc
		offset += len(docAsSlice)
		return section, offset, nil
		// Case 2: payload == 1
	case mgo.MsgPayload1:
		section.PayloadType = mgo.MsgPayload1
		payload, length, err := readPayloadType1(r)
		if err != nil {
			return mgo.MsgSection{}, 0, err
		}
		offset += length
		section.Data = payload
		return section, offset, nil
	default:
		return mgo.MsgSection{}, 0, fmt.Errorf("unknown payload type: %d", buf[0])
	}
}

func (msgOp *MsgOp) getDB() (string, error) {
	// if the msgOp DB was already set, return it
	if msgOp.Database != "" {
		return msgOp.Database, nil
	}
	if len(msgOp.Sections) == 0 {
		return "", fmt.Errorf("msg op contained no documents")
	}

	payload0DataRaw, _, err := fetchPayload0Data(msgOp.Sections)
	if err != nil {
		return "", err
	}

	var dbName string
	dbDoc := &struct {
		DB string `bson:"$db"`
	}{}

	err = payload0DataRaw.Unmarshal(dbDoc)
	if err != nil {
		return "", err
	}
	dbName = dbDoc.DB
	msgOp.Database = dbDoc.DB
	return dbName, nil
}

func (msgOp *MsgOp) getCommandName() (string, error) {
	// if the msgOp command name was already set, return it
	if msgOp.CommandName != "" {
		return msgOp.CommandName, nil
	}
	if len(msgOp.Sections) == 0 {
		return "", fmt.Errorf("msg op contained no documents")
	}
	payload0DataRaw, _, err := fetchPayload0Data(msgOp.Sections)
	if err != nil {
		return "", err
	}

	if len(payload0DataRaw.Data) < 5 {
		return "", fmt.Errorf("section contained no documents")
	}

	// Unmarshal into a RawD, a lightweight change that will allow access to the
	// name field of the bson documents
	rawD := bson.RawD{}
	payload0DataRaw.Unmarshal(&rawD)
	msgOp.CommandName = rawD[0].Name
	return msgOp.CommandName, nil
}

// readPayloadType1 takes an io.Reader and retrieves a single payload
// from it.
func readPayloadType1(r io.Reader) (mgo.PayloadType1, int, error) {
	var payload mgo.PayloadType1
	offset := 0
	buf := [4]byte{}
	// Read the payload size, 4 bytes
	_, err := io.ReadFull(r, buf[:])
	if err != nil {
		return payload, 0, err
	}
	payload.Size = getInt32(buf[:], 0)
	offset += 4

	//Read the identifier
	identifier, err := readCStringFromReader(r)
	if err != nil {
		return payload, 0, err
	}
	payload.Identifier = string(identifier)
	offset += len([]byte(identifier)) + 1

	//read all the present documents
	docs := []interface{}{}
	for payload.Size-int32(offset) > 0 {
		docAsSlice, err := ReadDocument(r)
		if err != nil {
			return payload, 0, err
		}

		doc := bson.Raw{}
		err = bson.Unmarshal(docAsSlice, &doc)
		if err != nil {
			return payload, 0, err
		}
		docs = append(docs, doc)
		offset += len(docAsSlice)
	}
	payload.Docs = docs
	return payload, offset, nil
}

func getCursorDocsFromMsgSection(section mgo.MsgSection) ([]bson.Raw, error) {
	var cursorDocAsRaw *bson.Raw
	switch section.PayloadType {
	case mgo.MsgPayload0:
		var ok bool
		cursorDocAsRaw, ok = section.Data.(*bson.Raw)
		if !ok {
			return []bson.Raw{}, fmt.Errorf("section data was wrong type")
		}
	}
	return getCursorDocs(cursorDocAsRaw)
}

func fetchPayload0Data(sections []mgo.MsgSection) (*bson.Raw, int, error) {
	for sectionIx, section := range sections {
		if section.PayloadType == mgo.MsgPayload0 {
			payload0DataRaw, ok := section.Data.(*bson.Raw)
			if ok {
				return payload0DataRaw, sectionIx, nil
			}
			return nil, 0, fmt.Errorf("data from payload of type 0 was wrong type")
		}
	}
	return nil, 0, fmt.Errorf("payload 0 not found")
}

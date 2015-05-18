package json

import (
	"fmt"
	"gopkg.in/mgo.v2/bson"
	"reflect"
)

// Represents base-64 encoded binary data
type BinData struct {
	Type   byte
	Base64 string
}

// Represents the number of milliseconds since the Unix epoch.
type Date int64

type ISODate string

type ObjectId string

// Represents a reference to another document.
type DBRef struct {
	Collection string
	Id         interface{}
	Database   string // optional
}

// Refers to a document in some namespace by wrapping a string containing the namespace
// and the objectId in which the _id of the document is contained
type DBPointer struct {
	Namespace string
	Id        bson.ObjectId
}

// Represents the literal MinKey.
type MinKey struct{}

// Represents the literal MaxKey.
type MaxKey struct{}

// Represents a signed 32-bit integer.
type NumberInt int32

// Represents a signed 64-bit integer.
type NumberLong int64

// Represents a signed 64-bit float.
type NumberFloat float64

// Represents a regular expression.
type RegExp struct {
	Pattern string
	Options string
}

// Represents a timestamp value.
type Timestamp struct {
	Seconds   uint32
	Increment uint32
}

type JavaScript struct {
	Code  string
	Scope interface{}
}

type Float float64

// Represents the literal undefined.
type Undefined struct{}

var (
	// primitive types
	byteType   = reflect.TypeOf(byte(0))
	stringType = reflect.TypeOf(string(""))
	uint32Type = reflect.TypeOf(uint32(0))

	// object types
	binDataType     = reflect.TypeOf(BinData{})
	dateType        = reflect.TypeOf(Date(0))
	isoDateType     = reflect.TypeOf(ISODate(""))
	dbRefType       = reflect.TypeOf(DBRef{})
	dbPointerType   = reflect.TypeOf(DBPointer{})
	maxKeyType      = reflect.TypeOf(MaxKey{})
	minKeyType      = reflect.TypeOf(MinKey{})
	numberIntType   = reflect.TypeOf(NumberInt(0))
	numberLongType  = reflect.TypeOf(NumberLong(0))
	numberFloatType = reflect.TypeOf(NumberFloat(0))
	objectIdType    = reflect.TypeOf(ObjectId(""))
	regexpType      = reflect.TypeOf(RegExp{})
	timestampType   = reflect.TypeOf(Timestamp{})
	undefinedType   = reflect.TypeOf(Undefined{})
	orderedBSONType = reflect.TypeOf(bson.D{})
	interfaceType   = reflect.TypeOf((*interface{})(nil))
)

func (d Date) isFormatable() bool {
	return int64(d) < int64(32535215999000)
}

func stateBeginExtendedValue(s *scanner, c int) int {
	switch c {
	case 'u': // beginning of undefined
		s.step = stateU
	case 'B': // beginning of BinData
		s.step = stateB
	case 'D': // beginning of Date
		s.step = stateD
	case 'I': // beginning of Infinity or ISODate
		s.step = stateI
	case 'M': // beginning of MinKey or MaxKey
		s.step = stateM
	case 'N': // beginning of NaN or NumberXX
		s.step = stateUpperN
	case 'O': // beginning of ObjectId
		s.step = stateO
	case 'R': // beginning of RegExp
		s.step = stateR
	case 'T': // beginning of Timestamp
		s.step = stateUpperT
	case '/': // beginning of /foo/i
		s.step = stateInRegexpPattern
	default:
		return s.error(c, "looking for beginning of value")
	}

	return scanBeginLiteral
}

// stateUpperN is the state after reading `N`.
func stateUpperN(s *scanner, c int) int {
	if c == 'a' {
		s.step = stateUpperNa
		return scanContinue
	}
	if c == 'u' {
		s.step = stateUpperNu
		return scanContinue
	}
	return s.error(c, "in literal NaN or Number (expecting 'a' or 'u')")
}

// stateM is the state after reading `M`.
func stateM(s *scanner, c int) int {
	if c == 'a' {
		s.step = stateUpperMa
		return scanContinue
	}
	if c == 'i' {
		s.step = stateUpperMi
		return scanContinue
	}
	return s.error(c, "in literal MaxKey or MinKey (expecting 'a' or 'i')")
}

// stateD is the state after reading `D`.
func stateD(s *scanner, c int) int {
	switch c {
	case 'a':
		s.step = stateDa
	case 'B':
		s.step = stateDB
	case 'b':
		s.step = stateDb
	default:
		return s.error(c, "in literal Date or DBRef (expecting 'a' or 'B')")
	}
	return scanContinue
}

// stateDB is the state after reading `DB`.
func stateDB(s *scanner, c int) int {
	if c == 'R' {
		s.step = stateDBR
		return scanContinue
	}
	if c == 'P' {
		s.step = stateDBP
		return scanContinue
	}
	return s.error(c, "in state DB (expecting 'R or P')")
}

// stateI is the state after reading `I`.
func stateI(s *scanner, c int) int {
	switch c {
	case 'n':
		s.step = stateIn
	case 'S':
		s.step = stateIS
	default:
		return s.error(c, "in literal Infinity or ISO (expecting 'n' or 'S')")
	}
	return scanContinue
}

// Decodes a literal stored in item into v.
func (d *decodeState) storeExtendedLiteral(item []byte, v reflect.Value, fromQuoted bool) bool {
	switch c := item[0]; c {
	case 'n':
		d.storeNewLiteral(v, fromQuoted)

	case 'u': // undefined
		switch kind := v.Kind(); kind {
		case reflect.Interface:
			v.Set(reflect.ValueOf(Undefined{}))
		default:
			d.error(fmt.Errorf("cannot store %v value into %v type", undefinedType, kind))
		}

	case 'B': // BinData
		d.storeBinData(v)

	case 'D': // Date, DBRef, DBPointer, Dbpointer,or Dbref
		switch item[1] {
		case 'a': // Date
			d.storeDate(v)
		case 'b': //Dbref
			d.storeDBRef(v)
		case 'B': // DBRef or DBPointer
			switch item[2] {
			case 'R': //DBRef
				d.storeDBRef(v)
			case 'P': //DBPointer
				d.storeDBPointer(v)
			}
		}
	case 'I':
		switch item[1] {
		case 'S': // ISODate
			d.storeISODate(v)
		}

	case 'M': // MinKey or MaxKey
		switch item[1] {
		case 'i': // MinKey
			switch kind := v.Kind(); kind {
			case reflect.Interface:
				v.Set(reflect.ValueOf(MinKey{}))
			default:
				d.error(fmt.Errorf("cannot store %v value into %v type", minKeyType, kind))
			}
		case 'a': // MaxKey
			switch kind := v.Kind(); kind {
			case reflect.Interface:
				v.Set(reflect.ValueOf(MaxKey{}))
			default:
				d.error(fmt.Errorf("cannot store %v value into %v type", maxKeyType, kind))
			}
		}

	case 'O': // ObjectId
		d.storeObjectId(v)

	case 'N': // NumberInt or NumberLong
		switch item[6] {
		case 'I': // NumberInt
			d.storeNumberInt(v)
		case 'L': // NumberLong
			d.storeNumberLong(v)
		}

	case 'R': // RegExp constructor
		d.storeRegexp(v)

	case 'T': // Timestamp
		d.storeTimestamp(v)

	case '/': // regular expression literal
		op := d.scanWhile(scanSkipSpace)
		if op != scanRegexpPattern {
			d.error(fmt.Errorf("expected beginning of regular expression pattern"))
		}

		pattern, options, err := d.regexp()
		if err != nil {
			d.error(err)
		}
		switch kind := v.Kind(); kind {
		case reflect.Interface:
			v.Set(reflect.ValueOf(RegExp{pattern, options}))
		default:
			d.error(fmt.Errorf("cannot store %v value into %v type", regexpType, kind))
		}

	default:
		return false
	}

	return true
}

// Returns a literal from the underlying byte data.
func (d *decodeState) getExtendedLiteral(item []byte) (interface{}, bool) {
	switch c := item[0]; c {
	case 'n':
		return d.getNewLiteral(), true

	case 'u': // undefined
		return Undefined{}, true

	case 'B': // BinData
		return d.getBinData(), true

	case 'D': // Date, DBRef, or Dbref
		switch item[1] {
		case 'a': // Date
			return d.getDate(), true
		case 'b': // Dbref
			return d.getDBRef(), true
		case 'B': // DBRef or DBPoiner
			switch item[2] {
			case 'R': // DBRef
				return d.getDBRef(), true
			case 'P': // DBPointer
				return d.getDBPointer(), true
			}
		}

	case 'M': // MinKey or MaxKey
		switch item[1] {
		case 'i': // MinKey
			return MinKey{}, true
		case 'a': // MaxKey
			return MaxKey{}, true
		}

	case 'O': // ObjectId
		return d.getObjectId(), true

	case 'N': // NumberInt or NumberLong
		switch item[6] {
		case 'I': // NumberInt
			return d.getNumberInt(), true
		case 'L': // NumberLong
			return d.getNumberLong(), true
		}

	case 'R': // RegExp constructor
		return d.getRegexp(), true

	case 'T': // Timestamp
		return d.getTimestamp(), true

	case 'I': // ISO Date
		switch item[1] {
		case 'S': // ISODate
			return d.getDate(), true
		}

	case '/': // regular expression literal
		op := d.scanWhile(scanSkipSpace)
		if op != scanRegexpPattern {
			d.error(fmt.Errorf("expected beginning of regular expression pattern"))
		}

		pattern, options, err := d.regexp()
		if err != nil {
			d.error(err)
		}
		return RegExp{pattern, options}, true
	}

	return nil, false
}

"""GDB Pretty-printers for MongoDB."""

import re
import struct
import sys
import uuid

import gdb.printing

try:
    import bson
    import bson.json_util
    import collections
    from bson.codec_options import CodecOptions
except ImportError as err:
    print("Warning: Could not load bson library for Python '" + str(sys.version) + "'.")
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None

if sys.version_info[0] < 3:
    raise gdb.GdbError(
        "MongoDB gdb extensions only support Python 3. Your GDB was compiled against Python 2")


def get_unique_ptr(obj):
    """Read the value of a libstdc++ std::unique_ptr."""
    return obj['_M_t']['_M_t']['_M_head_impl']


###################################################################################################
#
# Pretty-Printers
#
###################################################################################################


class StatusPrinter(object):
    """Pretty-printer for mongo::Status."""

    @staticmethod
    def extract_error(val):
        """Extract the error object (if any) from a Status/StatusWith."""
        error = val['_error']
        if 'px' in error.type.iterkeys():
            return error['px']
        return error

    @staticmethod
    def generate_error_details(error):
        """Generate a (code,reason) tuple from a Status/StatusWith error object."""
        info = error.dereference()
        code = info['code']
        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        return (code, info['reason'])

    def __init__(self, val):
        """Initialize StatusPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        error = StatusPrinter.extract_error(self.val)
        if not error:
            return 'Status::OK()'
        return 'Status(%s, %s)' % StatusPrinter.generate_error_details(error)


class StatusWithPrinter(object):
    """Pretty-printer for mongo::StatusWith<>."""

    def __init__(self, val):
        """Initialize StatusWithPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        error = StatusPrinter.extract_error(self.val['_status'])
        if not error:
            return 'StatusWith(OK, %s)' % (self.val['_t'])
        return 'StatusWith(%s, %s)' % StatusPrinter.generate_error_details(error)


class StringDataPrinter(object):
    """Pretty-printer for mongo::StringData."""

    def __init__(self, val):
        """Initialize StringDataPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'string'

    def to_string(self):
        """Return data for printing."""
        size = self.val["_size"]
        if size == -1:
            return self.val['_data'].lazy_string()
        return self.val['_data'].lazy_string(length=size)


class BSONObjPrinter(object):
    """Pretty-printer for mongo::BSONObj."""

    def __init__(self, val):
        """Initialize BSONObjPrinter."""
        self.val = val
        self.ptr = self.val['_objdata'].cast(gdb.lookup_type('void').pointer())
        self.is_valid = False

        # Handle the endianness of the BSON object size, which is represented as a 32-bit integer
        # in little-endian format.
        inferior = gdb.selected_inferior()
        if self.ptr.is_optimized_out:
            # If the value has been optimized out, we cannot decode it.
            self.size = -1
            self.raw_memory = None
        else:
            self.size = struct.unpack('<I', inferior.read_memory(self.ptr, 4))[0]
            self.raw_memory = bytes(memoryview(inferior.read_memory(self.ptr, self.size)))
            if bson:
                self.is_valid = bson.is_valid(self.raw_memory)

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'map'

    def children(self):
        """Children."""
        # Do not decode a BSONObj with an invalid size, or that is considered
        # invalid by pymongo.
        if not bson or not self.is_valid or self.size < 5 or self.size > 17 * 1024 * 1024:
            return

        options = CodecOptions(document_class=collections.OrderedDict)
        bsondoc = bson.decode(self.raw_memory, codec_options=options)

        for key, val in list(bsondoc.items()):
            yield 'key', key
            yield 'value', bson.json_util.dumps(val)

    def to_string(self):
        """Return BSONObj for printing."""
        # The value has been optimized out.
        if self.size == -1:
            return "BSONObj @ %s - optimized out" % (self.ptr)

        ownership = "owned" if self.val['_ownedBuffer']['_buffer']['_holder']['px'] else "unowned"

        size = self.size
        # Print an invalid BSONObj size in hex.
        if size < 5 or size > 17 * 1024 * 1024:
            size = hex(size)

        if size == 5:
            return "%s empty BSONObj @ %s" % (ownership, self.ptr)

        suffix = ""
        if not self.is_valid:
            # Wondering why this is unprintable? See PYTHON-1824. The Python
            # driver's BSON implementation does not support all possible BSON
            # datetimes. (specifically any BSON datetime where the year is >
            # datetime.MAXYEAR (usually 9999)).
            # Attempting to print any BSONObj that contains an out of range
            # datetime at any level of the document will cause an exception.
            # There exists no workaround for this in the driver; not even the
            # TypeDecoder API works for this because the BSON implementation
            # errors out early when the date is out of range.
            suffix = " - unprintable or invalid"
        return "%s BSONObj %s bytes @ %s%s" % (ownership, size, self.ptr, suffix)


class OplogEntryPrinter(object):
    """Pretty-printer for mongo::repl::OplogEntry."""

    def __init__(self, val):
        """Initialize OplogEntryPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'string'

    def to_string(self):
        """Return OplogEntry for printing."""
        optime = self.val['_entry']['_opTimeBase']
        optime_str = "ts(%s, %s)" % (optime['_timestamp']['secs'], optime['_timestamp']['i'])
        return "OplogEntry(%s, %s, %s, %s)" % (
            str(self.val['_entry']['_durableReplOperation']['_opType']).split('::')[-1],
            str(self.val['_entry']['_commandType']).split('::')[-1],
            self.val['_entry']['_durableReplOperation']['_nss']['_ns'], optime_str)


class UUIDPrinter(object):
    """Pretty-printer for mongo::UUID."""

    def __init__(self, val):
        """Initialize UUIDPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'string'

    def to_string(self):
        """Return UUID for printing."""
        raw_bytes = [self.val['_uuid']['_M_elems'][i] for i in range(16)]
        uuid_hex_bytes = [hex(int(b))[2:].zfill(2) for b in raw_bytes]
        return str(uuid.UUID("".join(uuid_hex_bytes)))


class OIDPrinter(object):
    """Pretty-printer for mongo::OID."""

    def __init__(self, val):
        """Initialize OIDPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'string'

    def to_string(self):
        """Return OID for printing."""
        raw_bytes = [int(self.val['_data'][i]) for i in range(12)]
        oid_hex_bytes = [hex(b & 0xFF)[2:].zfill(2) for b in raw_bytes]
        return "ObjectID('%s')" % "".join(oid_hex_bytes)


class RecordIdPrinter(object):
    """Pretty-printer for mongo::RecordId."""

    def __init__(self, val):
        """Initialize RecordIdPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'string'

    def to_string(self):
        """Return RecordId for printing."""
        rid_format = int(self.val["_format"])
        if rid_format == 0:
            return "null RecordId"
        elif rid_format == 1:
            hex_bytes = [int(self.val['_buffer'][i]) for i in range(8)]
            raw_bytes = bytes(hex_bytes)
            return "RecordId long: %s" % struct.unpack('l', raw_bytes)[0]
        elif rid_format == 2:
            str_len = int(self.val["_buffer"][0])
            raw_bytes = [int(self.val['_buffer'][i]) for i in range(1, str_len + 1)]
            hex_bytes = [hex(b & 0xFF)[2:].zfill(2) for b in raw_bytes]
            return "RecordId small string %d hex bytes: %s" % (str_len, str("".join(hex_bytes)))
        elif rid_format == 3:
            holder_ptr = self.val["_sharedBuffer"]["_buffer"]["_holder"]["px"]
            holder = holder_ptr.dereference()
            str_len = int(holder["_capacity"])
            # Start of data is immediately after pointer for holder
            start_ptr = (holder_ptr + 1).dereference().cast(gdb.lookup_type("char")).address
            raw_bytes = [int(start_ptr[i]) for i in range(1, str_len + 1)]
            hex_bytes = [hex(b & 0xFF)[2:].zfill(2) for b in raw_bytes]
            return "RecordId big string %d hex bytes @ %s: %s" % (str_len, holder_ptr + 1,
                                                                  str("".join(hex_bytes)))
        else:
            return "unknown RecordId format: %d" % rid_format


class DecorablePrinter(object):
    """Pretty-printer for mongo::Decorable<>."""

    def __init__(self, val):
        """Initialize DecorablePrinter."""
        self.val = val

        decl_vector = val["_decorations"]["_registry"]["_decorationInfo"]
        # TODO: abstract out navigating a std::vector
        self.start = decl_vector["_M_impl"]["_M_start"]
        finish = decl_vector["_M_impl"]["_M_finish"]
        decorable_t = val.type.template_argument(0)
        decinfo_t = gdb.lookup_type('mongo::DecorationRegistry<{}>::DecorationInfo'.format(
            str(decorable_t).replace("class", "").strip()))
        self.count = int((int(finish) - int(self.start)) / decinfo_t.sizeof)

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'map'

    def to_string(self):
        """Return Decorable for printing."""
        return "Decorable<%s> with %s elems " % (self.val.type.template_argument(0), self.count)

    def children(self):
        """Children."""
        decoration_data = get_unique_ptr(self.val["_decorations"]["_decorationData"])

        for index in range(self.count):
            descriptor = self.start[index]
            dindex = int(descriptor["descriptor"]["_index"])

            # In order to get the type stored in the decorable, we examine the type of its
            # constructor, and do some string manipulations.
            # TODO: abstract out navigating a std::function
            type_name = str(descriptor["constructor"])
            type_name = type_name[0:len(type_name) - 1]
            type_name = type_name[0:type_name.rindex(">")]
            type_name = type_name[type_name.index("constructAt<"):].replace("constructAt<", "")

            # If the type is a pointer type, strip the * at the end.
            if type_name.endswith('*'):
                type_name = type_name[0:len(type_name) - 1]
            type_name = type_name.rstrip()

            # Cast the raw char[] into the actual object that is stored there.
            type_t = gdb.lookup_type(type_name)
            obj = decoration_data[dindex].cast(type_t)

            yield ('key', "%d:%s:%s" % (index, obj.address, type_name))
            yield ('value', obj)


def _get_flags(flag_val, flags):
    """
    Return a list of flag name strings.

    `flags` is a list of `(flag_name, flag_value)` pairs. The list must be in sorted in order of the
    highest `flag_value` first and the lowest last.
    """
    if not flags:
        return "Flags not parsed from source."

    ret = []
    for name, hex_val in flags:
        dec_val = int(hex_val, 16)
        if flag_val < dec_val:
            continue

        ret.append(name)
        flag_val -= dec_val

    return ret


class WtCursorPrinter(object):
    """
    Pretty-printer for WT_CURSOR objects.

    Complement the `flags: int` field with the macro names used in the source code.
    """

    try:
        with open("./src/third_party/wiredtiger/src/include/wiredtiger.in") as wiredtiger_header:
            file_contents = wiredtiger_header.read()
            cursor_flags_re = re.compile(r"#define\s+WT_CURSTD_(\w+)\s+0x(\d+)u")
            cursor_flags = cursor_flags_re.findall(file_contents)[::-1]
    except IOError:
        cursor_flags = []

    def __init__(self, val):
        """Initializer."""
        self.val = val

    # pylint: disable=R0201
    def to_string(self):
        """to_string."""
        return None

    def children(self):
        """children."""
        for field in self.val.type.fields():
            field_val = self.val[field.name]
            if field.name == "flags":
                yield ("flags", "{} ({})".format(field_val,
                                                 str(_get_flags(field_val, self.cursor_flags))))
            else:
                yield (field.name, field_val)


class WtSessionImplPrinter(object):
    """
    Pretty-printer for WT_SESSION_IMPL objects.

    Complement the `flags: int` field with the macro names used in the source code.
    """

    try:
        with open("./src/third_party/wiredtiger/src/include/session.h") as session_header:
            file_contents = session_header.read()
            session_flags_re = re.compile(r"#define\s+WT_SESSION_(\w+)\s+0x(\d+)u")
            session_flags = session_flags_re.findall(file_contents)[::-1]
    except IOError:
        session_flags = []

    def __init__(self, val):
        """Initializer."""
        self.val = val

    # pylint: disable=R0201
    def to_string(self):
        """to_string."""
        return None

    def children(self):
        """children."""
        for field in self.val.type.fields():
            field_val = self.val[field.name]
            if field.name == "flags":
                yield ("flags", "{} ({})".format(field_val,
                                                 str(_get_flags(field_val, self.session_flags))))
            else:
                yield (field.name, field_val)


class WtTxnPrinter(object):
    """
    Pretty-printer for WT_TXN objects.

    Complement the `flags: int` field with the macro names used in the source code.
    """

    try:
        with open("./src/third_party/wiredtiger/src/include/txn.h") as txn_header:
            file_contents = txn_header.read()
            txn_flags_re = re.compile(r"#define\s+WT_TXN_(\w+)\s+0x(\d+)u")
            txn_flags = txn_flags_re.findall(file_contents)[::-1]
    except IOError:
        txn_flags = []

    def __init__(self, val):
        """Initializer."""
        self.val = val

    # pylint: disable=R0201
    def to_string(self):
        """to_string."""
        return None

    def children(self):
        """children."""
        for field in self.val.type.fields():
            field_val = self.val[field.name]
            if field.name == "flags":
                yield ("flags", "{} ({})".format(field_val,
                                                 str(_get_flags(field_val, self.txn_flags))))
            else:
                yield (field.name, field_val)


def absl_get_nodes(val):
    """Return a generator of every node in absl::container_internal::raw_hash_set and derived classes."""
    size = val["size_"]

    if size == 0:
        return

    table = val
    capacity = int(table["capacity_"])
    ctrl = table["ctrl_"]

    # Using the array of ctrl bytes, search for in-use slots and return them
    # https://github.com/abseil/abseil-cpp/blob/7ffbe09f3d85504bd018783bbe1e2c12992fe47c/absl/container/internal/raw_hash_set.h#L787-L788
    for item in range(capacity):
        ctrl_t = int(ctrl[item])
        if ctrl_t >= 0:
            yield table["slots_"][item]


class AbslHashSetPrinterBase(object):
    """Pretty-printer base class for absl::[node/flat]_hash_set<>."""

    def __init__(self, val, to_str):
        """Initialize absl::[node/flat]_hash_set."""
        self.val = val
        self.to_str = to_str

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'array'

    def to_string(self):
        """Return absl::[node/flat]_hash_set for printing."""
        return "absl::%s_hash_set<%s> with %s elems " % (
            self.to_str, self.val.type.template_argument(0), self.val["size_"])


class AbslNodeHashSetPrinter(AbslHashSetPrinterBase):
    """Pretty-printer for absl::node_hash_set<>."""

    def __init__(self, val):
        """Initialize absl::node_hash_set."""
        AbslHashSetPrinterBase.__init__(self, val, "node")

    def children(self):
        """Children."""
        count = 0
        for val in absl_get_nodes(self.val):
            yield (str(count), val.dereference())
            count += 1


class AbslFlatHashSetPrinter(AbslHashSetPrinterBase):
    """Pretty-printer for absl::flat_hash_set<>."""

    def __init__(self, val):
        """Initialize absl::flat_hash_set."""
        AbslHashSetPrinterBase.__init__(self, val, "flat")

    def children(self):
        """Children."""
        count = 0
        for val in absl_get_nodes(self.val):
            yield (str(count), val.reference_value())
            count += 1


class AbslHashMapPrinterBase(object):
    """Pretty-printer base class for absl::[node/flat]_hash_map<>."""

    def __init__(self, val, to_str):
        """Initialize absl::[node/flat]_hash_map."""
        self.val = val
        self.to_str = to_str

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'map'

    def to_string(self):
        """Return absl::[node/flat]_hash_map for printing."""
        return "absl::%s_hash_map<%s, %s> with %s elems " % (
            self.to_str, self.val.type.template_argument(0), self.val.type.template_argument(1),
            self.val["size_"])


class AbslNodeHashMapPrinter(AbslHashMapPrinterBase):
    """Pretty-printer for absl::node_hash_map<>."""

    def __init__(self, val):
        """Initialize absl::node_hash_map."""
        AbslHashMapPrinterBase.__init__(self, val, "node")

    def children(self):
        """Children."""
        for kvp in absl_get_nodes(self.val):
            yield ('key', kvp['first'])
            yield ('value', kvp['second'])


class AbslFlatHashMapPrinter(AbslHashMapPrinterBase):
    """Pretty-printer for absl::flat_hash_map<>."""

    def __init__(self, val):
        """Initialize absl::flat_hash_map."""
        AbslHashMapPrinterBase.__init__(self, val, "flat")

    def children(self):
        """Children."""
        for kvp in absl_get_nodes(self.val):
            yield ('key', kvp['key'])
            yield ('value', kvp['value'])


def find_match_brackets(search, opening='<', closing='>'):
    """Return the index of the closing bracket that matches the first opening bracket.

    Return -1 if no last matching bracket is found, i.e. not a template.

    Example:
        'Foo<T>::iterator<U>''
        returns 5

    """
    index = search.find(opening)
    if index == -1:
        return -1

    start = index + 1
    count = 1
    str_len = len(search)
    for index in range(start, str_len):
        char = search[index]

        if char == opening:
            count += 1
        elif char == closing:
            count -= 1

        if count == 0:
            return index

    return -1


class MongoSubPrettyPrinter(gdb.printing.SubPrettyPrinter):
    """Sub pretty printer managed by the pretty-printer collection."""

    def __init__(self, name, prefix, is_template, printer):
        """Initialize MongoSubPrettyPrinter."""
        super(MongoSubPrettyPrinter, self).__init__(name)
        self.prefix = prefix
        self.printer = printer
        self.is_template = is_template


class MongoPrettyPrinterCollection(gdb.printing.PrettyPrinter):
    """MongoDB-specific printer printer collection that ignores subtypes.

    It will match 'HashTable<T> but not 'HashTable<T>::iterator' when asked for 'HashTable'.
    """

    def __init__(self):
        """Initialize MongoPrettyPrinterCollection."""
        super(MongoPrettyPrinterCollection, self).__init__("mongo", [])

    def add(self, name, prefix, is_template, printer):
        """Add a subprinter."""
        self.subprinters.append(MongoSubPrettyPrinter(name, prefix, is_template, printer))

    def __call__(self, val):
        """Return matched printer type."""

        # Get the type name.
        lookup_tag = gdb.types.get_basic_type(val.type).tag
        if not lookup_tag:
            lookup_tag = val.type.name
        if not lookup_tag:
            return None

        index = find_match_brackets(lookup_tag)

        for printer in self.subprinters:
            if not printer.enabled:
                continue
            # Ignore subtypes of templated classes.
            # We do not want HashTable<T>::iterator as an example, just HashTable<T>
            if printer.is_template:
                if (index + 1 == len(lookup_tag) and lookup_tag.find(printer.prefix) == 0):
                    return printer.printer(val)
            elif lookup_tag == printer.prefix:
                return printer.printer(val)

        return None


class WtUpdateToBsonPrinter(object):
    """Pretty printer for WT_UPDATE. Interpreting the `data` field as bson."""

    def __init__(self, val):
        """Initializer."""
        self.val = val
        self.size = self.val['size']
        self.ptr = self.val['data']

    @staticmethod
    def display_hint():
        """DisplayHint."""
        return 'map'

    # pylint: disable=R0201
    def to_string(self):
        """ToString."""
        elems = []
        for idx in range(len(self.val.type.fields())):
            fld = self.val.type.fields()[idx]
            val = self.val[fld.name]
            elems.append(str((fld.name, str(val))))
        return "WT_UPDATE: \n  %s" % ('\n  '.join(elems))

    def children(self):
        """children."""
        memory = gdb.selected_inferior().read_memory(self.ptr, self.size).tobytes()
        bsonobj = next(bson.decode_iter(memory))  # pylint: disable=stop-iteration-return

        for key, value in list(bsonobj.items()):
            yield 'key', key
            yield 'value', bson.json_util.dumps(value)


def make_inverse_enum_dict(enum_type_name):
    """
    Create a dictionary that maps enum values to the unqualified names of the enum elements.

    For example, if the enum type is 'mongo::sbe::vm::Builtin' with an element 'regexMatch', the
    dictionary will contain 'regexMatch' value and not 'mongo::sbe::vm::Builtin::regexMatch'.
    """
    enum_dict = gdb.types.make_enum_dict(gdb.lookup_type(enum_type_name))
    enum_inverse_dic = dict()
    for key, value in enum_dict.items():
        enum_inverse_dic[int(value)] = key.split('::')[-1]  # take last element
    return enum_inverse_dic


def read_as_integer(pmem, size):
    """Read 'size' bytes at 'pmem' as an integer."""
    # We assume the same platform for the debugger and the debuggee (thus, 'sys.byteorder'). If
    # this becomes a problem look into whether it's possible to determine the byteorder of the
    # inferior.
    return int.from_bytes( \
        gdb.selected_inferior().read_memory(pmem, size).tobytes(), \
        sys.byteorder)


class SbeCodeFragmentPrinter(object):
    """
    Pretty-printer for mongo::sbe::vm::CodeFragment.

    Objects of 'mongo::sbe::vm::CodeFragment' type contain a stream of op-codes to be executed by
    the 'sbe::vm::ByteCode' class. The pretty printer decodes the stream and outputs it as a list of
    named instructions.
    """

    def __init__(self, val):
        """Initialize SbeCodeFragmentPrinter."""
        self.val = val

        # The instructions stream is stored using 'absl::InlinedVector<uint8_t, 16>' type, which can
        # either use an inline buffer or an allocated one. The choice of storage is decoded in the
        # last bit of the 'metadata_' field.
        storage = self.val['_instrs']['storage_']
        meta = storage['metadata_'].cast(gdb.lookup_type('size_t'))
        self.is_inlined = (meta % 2 == 0)
        self.size = (meta >> 1)
        self.pdata = \
            storage['data_']['inlined']['inlined_data'].cast(gdb.lookup_type('uint8_t').pointer()) \
            if self.is_inlined \
            else storage['data_']['allocated']['allocated_data']

        # Precompute lookup tables for Instructions and Builtins.
        self.optags_lookup = make_inverse_enum_dict('mongo::sbe::vm::Instruction::Tags')
        self.builtins_lookup = make_inverse_enum_dict('mongo::sbe::vm::Builtin')
        self.valuetags_lookup = make_inverse_enum_dict('mongo::sbe::value::TypeTags')

    def to_string(self):
        """Return sbe::vm::CodeFragment for printing."""
        return "%s" % (self.val.type)

    # pylint: disable=R0915
    def children(self):
        """children."""
        yield '_instrs', '{... (to see raw output, run "disable pretty-printer")}'
        yield '_fixUps', self.val['_fixUps']
        yield '_stackSize', self.val['_stackSize']

        yield 'inlined', self.is_inlined
        yield 'instrs data at', '[{} - {}]'.format(hex(self.pdata), hex(self.pdata + self.size))
        yield 'instrs total size', self.size

        # Sizes for types we'll use when parsing the insructions stream.
        int_size = gdb.lookup_type('int').sizeof
        ptr_size = gdb.lookup_type('void').pointer().sizeof
        tag_size = gdb.lookup_type('mongo::sbe::value::TypeTags').sizeof
        value_size = gdb.lookup_type('mongo::sbe::value::Value').sizeof
        uint8_size = gdb.lookup_type('uint8_t').sizeof
        uint32_size = gdb.lookup_type('uint32_t').sizeof
        builtin_size = gdb.lookup_type('mongo::sbe::vm::Builtin').sizeof

        cur_op = self.pdata
        end_op = self.pdata + self.size
        instr_count = 0
        error = False
        while cur_op < end_op:
            op_addr = cur_op
            op_tag = read_as_integer(op_addr, 1)

            if not op_tag in self.optags_lookup:
                yield hex(op_addr), 'unknown op tag: {}'.format(op_tag)
                error = True
                break
            op_name = self.optags_lookup[op_tag]

            cur_op += 1
            instr_count += 1

            # Some instructions have extra arguments, embedded into the ops stream.
            args = ''
            if op_name in ['pushLocalVal', 'pushMoveLocalVal', 'pushLocalLambda', 'traversePConst']:
                args = 'arg: ' + str(read_as_integer(cur_op, int_size))
                cur_op += int_size
            elif op_name in ['jmp', 'jmpTrue', 'jmpNothing']:
                offset = read_as_integer(cur_op, int_size)
                cur_op += int_size
                args = 'offset: ' + str(offset) + ', target: ' + hex(cur_op + offset)
            elif op_name in ['pushConstVal', 'getFieldConst']:
                tag = read_as_integer(cur_op, tag_size)
                args = 'tag: ' + self.valuetags_lookup.get(tag, "unknown") + \
                    ', value: ' + hex(read_as_integer(cur_op + tag_size, value_size))
                cur_op += (tag_size + value_size)
            elif op_name in ['pushAccessVal', 'pushMoveVal']:
                args = 'accessor: ' + hex(read_as_integer(cur_op, ptr_size))
                cur_op += ptr_size
            elif op_name in ['numConvert']:
                args = 'convert to: ' + \
                    self.valuetags_lookup.get(read_as_integer(cur_op, tag_size), "unknown")
                cur_op += tag_size
            elif op_name in ['typeMatch']:
                args = 'mask: ' + hex(read_as_integer(cur_op, uint32_size))
                cur_op += uint32_size
            elif op_name in ['function', 'functionSmall']:
                arity_size = \
                    gdb.lookup_type('mongo::sbe::vm::ArityType').sizeof \
                    if op_name == 'function' \
                    else gdb.lookup_type('mongo::sbe::vm::SmallArityType').sizeof
                builtin_id = read_as_integer(cur_op, builtin_size)
                args = 'builtin: ' + self.builtins_lookup.get(builtin_id, "unknown")
                args += ' arity: ' + str(read_as_integer(cur_op + builtin_size, arity_size))
                cur_op += (builtin_size + arity_size)
            elif op_name in ['fillEmptyConst']:
                args = 'Instruction::Constants: ' + str(read_as_integer(cur_op, uint8_size))
                cur_op += uint8_size
            elif op_name in ['traverseFConst']:
                const_enum = read_as_integer(cur_op, uint8_size)
                cur_op += uint8_size
                args = \
                    'Instruction::Constants: ' + str(const_enum) + \
                    ", offset: " + str(read_as_integer(cur_op, int_size))
            elif op_name in ['applyClassicMatcher']:
                args = 'MatchExpression* ' + hex(read_as_integer(cur_op, ptr_size))
                cur_op += ptr_size

            yield hex(op_addr), '{} ({})'.format(op_name, args)

        yield 'instructions count', \
            instr_count if not error else '? (successfully parsed {})'.format(instr_count)


def eval_print_fn(val, print_fn):
    """Evaluate a print function, and return the resulting string."""

    # The generated output from explain contains the string "\n" (two characters)
    # replace them with a single EOL character so that GDB prints multi-line
    # explains nicely.
    pp_result = print_fn(val)
    pp_str = str(pp_result).replace("\"", "").replace("\\n", "\n")
    return pp_str


class ABTPrinter(object):
    """Pretty-printer for mongo::optimizer::ABT."""

    def __init__(self, val):
        """Initialize ABTPrinter."""
        self.val = val
        (print_fn_symbol, _) = gdb.lookup_symbol("ExplainGenerator::explainNode")
        if print_fn_symbol is None:
            raise gdb.GdbError("Could not find ABT print function")
        self.print_fn = print_fn_symbol.value()

    @staticmethod
    def display_hint():
        """Display hint."""
        # Return None here to allow formatting of the resulting string with newline characters.
        return None

    def to_string(self):
        """Return ABT for printing."""
        # Do not enable these printers when printing of frame-arguments is enabled, as it can crash
        # GDB or lead to stack overflow. See https://sourceware.org/bugzilla/show_bug.cgi?id=28856
        # for more details.
        if gdb.parameter("print frame-arguments") != "none":
            print("\nWarning: ABT pretty printers disabled, run `set print frame-arguments none`" +
                  " and `source mongo_printers.py` to enable\n")
            return "%s" % "<ABT>"

        # Python will truncate/compress certain strings that contain many repeated characters.
        # For an ABT, this is quite common when indenting nodes to represent children, so
        # warn the user depending on the current settings.
        print_repeats = gdb.parameter("print repeats")
        print_repeats = "unlimited" if print_repeats is None else print_repeats
        print_elements = gdb.parameter("print elements")
        print_elements = "unlimited" if print_elements is None else print_elements
        if print_repeats not in (0, "unlimited") or print_elements not in (0, "unlimited"):
            print(
                "\n**Warning: recommend setting `print repeats` and `print elements` to 0 when printing an ABT**\n"
            )
        res = eval_print_fn(self.val, self.print_fn)
        return "%s" % (res)


class OptimizerTypePrinter(object):
    """Base class that pretty prints via a single argument C++ function."""

    def __init__(self, val, print_fn_name):
        """Initialize base printer."""
        self.val = val
        (print_fn_symbol, _) = gdb.lookup_symbol(print_fn_name)
        if print_fn_symbol is None:
            raise gdb.GdbError("Could not find pretty print function: " + print_fn_name)
        self.print_fn = print_fn_symbol.value()

    @staticmethod
    def display_hint():
        """Display hint."""
        return None

    def to_string(self):
        """Return string for printing."""
        return eval_print_fn(self.val, self.print_fn)


class IntervalPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::IntervalRequirement."""

    def __init__(self, val):
        """Initialize IntervalPrinter."""
        super().__init__(val, "ExplainGenerator::explainInterval")


class PartialSchemaReqMapPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::PartialSchemaRequirements."""

    def __init__(self, val):
        """Initialize PartialSchemaReqMapPrinter."""
        super().__init__(val, "ExplainGenerator::explainPartialSchemaReqMap")


class MemoPrinter(OptimizerTypePrinter):
    """Pretty-printer for mongo::optimizer::cascades::Memo."""

    def __init__(self, val):
        """Initialize MemoPrinter."""
        super().__init__(val, "ExplainGenerator::explainMemo")


def register_abt_printers(pp):
    """Registers a number of pretty printers related to the CQF optimizer."""

    try:
        # ABT printer.
        abt_type = gdb.lookup_type("mongo::optimizer::ABT").strip_typedefs()
        pp.add('ABT', abt_type.name, False, ABTPrinter)

        abt_ref_type = gdb.lookup_type(abt_type.name + "::Reference").strip_typedefs()
        # We can re-use the same printer since an ABT is contructable from an ABT::Reference.
        pp.add('ABT::Reference', abt_ref_type.name, False, ABTPrinter)

        # IntervalRequirement printer.
        pp.add("Interval", "mongo::optimizer::IntervalRequirement", False, IntervalPrinter)

        # PartialSchemaRequirements printer.
        schema_req_type = gdb.lookup_type(
            "mongo::optimizer::PartialSchemaRequirements").strip_typedefs()
        pp.add("PartialSchemaRequirements", schema_req_type.name, False, PartialSchemaReqMapPrinter)

        # Memo printer.
        pp.add("Memo", "mongo::optimizer::cascades::Memo", False, MemoPrinter)
    except gdb.error as gdberr:
        print("Failed to add one or more ABT pretty printers, skipping: " + str(gdberr))


def build_pretty_printer():
    """Build a pretty printer."""
    pp = MongoPrettyPrinterCollection()
    pp.add('BSONObj', 'mongo::BSONObj', False, BSONObjPrinter)
    pp.add('Decorable', 'mongo::Decorable', True, DecorablePrinter)
    pp.add('Status', 'mongo::Status', False, StatusPrinter)
    pp.add('StatusWith', 'mongo::StatusWith', True, StatusWithPrinter)
    pp.add('StringData', 'mongo::StringData', False, StringDataPrinter)
    pp.add('node_hash_map', 'absl::lts_20210324::node_hash_map', True, AbslNodeHashMapPrinter)
    pp.add('node_hash_set', 'absl::lts_20210324::node_hash_set', True, AbslNodeHashSetPrinter)
    pp.add('flat_hash_map', 'absl::lts_20210324::flat_hash_map', True, AbslFlatHashMapPrinter)
    pp.add('flat_hash_set', 'absl::lts_20210324::flat_hash_set', True, AbslFlatHashSetPrinter)
    pp.add('RecordId', 'mongo::RecordId', False, RecordIdPrinter)
    pp.add('UUID', 'mongo::UUID', False, UUIDPrinter)
    pp.add('OID', 'mongo::OID', False, OIDPrinter)
    pp.add('OplogEntry', 'mongo::repl::OplogEntry', False, OplogEntryPrinter)
    pp.add('__wt_cursor', '__wt_cursor', False, WtCursorPrinter)
    pp.add('__wt_session_impl', '__wt_session_impl', False, WtSessionImplPrinter)
    pp.add('__wt_txn', '__wt_txn', False, WtTxnPrinter)
    pp.add('__wt_update', '__wt_update', False, WtUpdateToBsonPrinter)
    pp.add('CodeFragment', 'mongo::sbe::vm::CodeFragment', False, SbeCodeFragmentPrinter)

    # Optimizer/ABT related pretty printers that can be used only with a running process.
    register_abt_printers(pp)

    return pp


###################################################################################################
#
# Setup
#
###################################################################################################

# Register pretty-printers, replace existing mongo printers
gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer(), True)

print("MongoDB GDB pretty-printers loaded")

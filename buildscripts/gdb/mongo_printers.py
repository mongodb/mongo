"""GDB Pretty-printers for MongoDB."""
from __future__ import print_function

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

if sys.version_info[0] >= 3:
    # GDB only permits converting a gdb.Value instance to its numerical address when using the
    # long() constructor in Python 2 and not when using the int() constructor. We define the
    # 'long' class as an alias for the 'int' class in Python 3 for compatibility.
    long = int  # pylint: disable=redefined-builtin,invalid-name


def get_unique_ptr(obj):
    """Read the value of a libstdc++ std::unique_ptr."""
    return obj["_M_t"]['_M_head_impl']


###################################################################################################
#
# Pretty-Printers
#
###################################################################################################


class StatusPrinter(object):
    """Pretty-printer for mongo::Status."""

    def __init__(self, val):
        """Initialize StatusPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        if not self.val['_error']:
            return 'Status::OK()'

        code = self.val['_error']['code']
        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_error'].dereference()
        reason = info['reason']
        return 'Status(%s, %s)' % (code, reason)


class StatusWithPrinter(object):
    """Pretty-printer for mongo::StatusWith<>."""

    def __init__(self, val):
        """Initialize StatusWithPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        if not self.val['_status']['_error']:
            return 'StatusWith(OK, %s)' % (self.val['_t'])

        code = self.val['_status']['_error']['code']

        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_status']['_error'].dereference()
        reason = info['reason']
        return 'StatusWith(%s, %s)' % (code, reason)


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
        # Handle the endianness of the BSON object size, which is represented as a 32-bit integer
        # in little-endian format.
        inferior = gdb.selected_inferior()
        if self.ptr.is_optimized_out:
            # If the value has been optimized out, we cannot decode it.
            self.size = -1
        else:
            self.size = struct.unpack('<I', inferior.read_memory(self.ptr, 4))[0]

    @staticmethod
    def display_hint():
        """Display hint."""
        return 'map'

    def children(self):
        """Children."""
        # Do not decode a BSONObj with an invalid size.
        if not bson or self.size < 5 or self.size > 17 * 1024 * 1024:
            return

        inferior = gdb.selected_inferior()
        buf = bson.BSON(bytes(inferior.read_memory(self.ptr, self.size)))
        options = CodecOptions(document_class=collections.OrderedDict)
        bsondoc = buf.decode(codec_options=options)

        for key, val in bsondoc.items():
            yield 'key', key
            yield 'value', bson.json_util.dumps(val)

    def to_string(self):
        """Return BSONObj for printing."""
        # The value has been optimized out.
        if self.size == -1:
            return "BSONObj @ %s" % (self.ptr)

        ownership = "owned" if self.val['_ownedBuffer']['_buffer']['_holder']['px'] else "unowned"

        size = self.size
        # Print an invalid BSONObj size in hex.
        if size < 5 or size > 17 * 1024 * 1024:
            size = hex(size)

        if size == 5:
            return "%s empty BSONObj @ %s" % (ownership, self.ptr)
        return "%s BSONObj %s bytes @ %s" % (ownership, size, self.ptr)


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
        self.count = long((long(finish) - long(self.start)) / decinfo_t.sizeof)

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
        return "absl::%s_hash_set<%s> with %s elems " % (self.to_str,
                                                         self.val.type.template_argument(0),
                                                         self.val["size_"])


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
        return "absl::%s_hash_map<%s, %s> with %s elems " % (self.to_str,
                                                             self.val.type.template_argument(0),
                                                             self.val.type.template_argument(1),
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

        # Ignore subtypes of classes
        # We do not want HashTable<T>::iterator as an example, just HashTable<T>
        if index == -1 or index + 1 == len(lookup_tag):
            for printer in self.subprinters:
                if not printer.enabled:
                    continue
                if ((not printer.is_template or lookup_tag.find(printer.prefix) != 0)
                        and (printer.is_template or lookup_tag != printer.prefix)):
                    continue
                return printer.printer(val)

        return None


def build_pretty_printer():
    """Build a pretty printer."""
    pp = MongoPrettyPrinterCollection()
    pp.add('BSONObj', 'mongo::BSONObj', False, BSONObjPrinter)
    pp.add('Decorable', 'mongo::Decorable', True, DecorablePrinter)
    pp.add('Status', 'mongo::Status', False, StatusPrinter)
    pp.add('StatusWith', 'mongo::StatusWith', True, StatusWithPrinter)
    pp.add('StringData', 'mongo::StringData', False, StringDataPrinter)
    pp.add('node_hash_map', 'absl::node_hash_map', True, AbslNodeHashMapPrinter)
    pp.add('node_hash_set', 'absl::node_hash_set', True, AbslNodeHashSetPrinter)
    pp.add('flat_hash_map', 'absl::flat_hash_map', True, AbslFlatHashMapPrinter)
    pp.add('flat_hash_set', 'absl::flat_hash_set', True, AbslFlatHashSetPrinter)
    pp.add('UUID', 'mongo::UUID', False, UUIDPrinter)
    pp.add('__wt_cursor', '__wt_cursor', False, WtCursorPrinter)
    pp.add('__wt_session_impl', '__wt_session_impl', False, WtSessionImplPrinter)
    pp.add('__wt_txn', '__wt_txn', False, WtTxnPrinter)
    return pp


###################################################################################################
#
# Setup
#
###################################################################################################

# Register pretty-printers, replace existing mongo printers
gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer(), True)

print("MongoDB GDB pretty-printers loaded")

"""GDB Pretty-printers for MongoDB."""

import os
import re
import struct
import sys
import uuid
from pathlib import Path

import gdb
import gdb.printing

ROOT_PATH = str(Path(os.path.abspath(__file__)).parent.parent.parent)
if ROOT_PATH not in sys.path:
    sys.path.insert(0, ROOT_PATH)
from src.third_party.immer.dist.tools.gdb_pretty_printers.printers import (
    ListIter as ImmerListIter,
)

if not gdb:
    from buildscripts.gdb.mongo import (
        get_boost_optional,
        get_decorable_info,
        get_object_decoration,
        lookup_type,
    )
    from buildscripts.gdb.optimizer_printers import register_optimizer_printers

try:
    import collections

    import bson
    import bson.json_util
    from bson.codec_options import CodecOptions
except ImportError:
    print("Warning: Could not load bson library for Python '" + str(sys.version) + "'.")
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None

if sys.version_info[0] < 3:
    raise gdb.GdbError(
        "MongoDB gdb extensions only support Python 3. Your GDB was compiled against Python 2"
    )


def get_unique_ptr_bytes(obj):
    """Read the value of a libstdc++ std::unique_ptr.

    Returns a gdb.Value where its type resolves to `unsigned char*`. The caller must take care to
    cast the returned value themselves. This function is particularly useful in the context of
    mongo::Decorable<> types which store the decorations as a slab of memory with
    std::unique_ptr<unsigned char[]>. In all other cases get_unique_ptr() can be preferred.
    """
    return obj.cast(gdb.lookup_type("std::_Head_base<0, unsigned char*, false>"))["_M_head_impl"]


def get_unique_ptr(obj):
    """Read the value of a libstdc++ std::unique_ptr."""
    return get_unique_ptr_bytes(obj).cast(obj.type.template_argument(0).pointer())


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
        error = val["_error"]
        if "px" in error.type.iterkeys():
            return error["px"]
        return error

    @staticmethod
    def generate_error_details(error):
        """Generate a (code,reason) tuple from a Status/StatusWith error object."""
        info = error.dereference()
        code = info["code"]
        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split("::")[-1]

        return (code, info["reason"])

    def __init__(self, val):
        """Initialize StatusPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        error = StatusPrinter.extract_error(self.val)
        if not error:
            return "Status::OK()"
        return "Status(%s, %s)" % StatusPrinter.generate_error_details(error)


class StatusWithPrinter(object):
    """Pretty-printer for mongo::StatusWith<>."""

    def __init__(self, val):
        """Initialize StatusWithPrinter."""
        self.val = val

    def to_string(self):
        """Return status for printing."""
        error = StatusPrinter.extract_error(self.val["_status"])
        if not error:
            return "StatusWith(OK, %s)" % (self.val["_t"])
        return "StatusWith(%s, %s)" % StatusPrinter.generate_error_details(error)


class StringDataPrinter(object):
    """Pretty-printer for mongo::StringData."""

    def __init__(self, val):
        """Initialize StringDataPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return "string"

    def to_string(self):
        """Return data for printing."""
        # As of SERVER-82604, StringData is based on std::string_view, so try with that first
        sv = self.val["_sv"]
        if sv is not None:
            return sv

        # ... back-off to the legacy format otherwise
        size = self.val["_size"]
        if size == -1:
            return self.val["_data"].lazy_string()
        return self.val["_data"].lazy_string(length=size)


class BoostOptionalPrinter(object):
    """Pretty-printer for boost::optional."""

    def __init__(self, val):
        """Initialize BoostOptionalPriner."""
        self.val = val

    def to_string(self):
        """Return data for printing."""
        return get_boost_optional(self.val)


class BSONObjPrinter(object):
    """Pretty-printer for mongo::BSONObj."""

    def __init__(self, val):
        """Initialize BSONObjPrinter."""
        self.val = val
        self.ptr = self.val["_objdata"].cast(lookup_type("void").pointer())
        self.is_valid = False

        # Handle the endianness of the BSON object size, which is represented as a 32-bit integer
        # in little-endian format.
        inferior = gdb.selected_inferior()
        if self.ptr.is_optimized_out:
            # If the value has been optimized out, we cannot decode it.
            self.size = -1
            self.raw_memory = None
        else:
            self.size = struct.unpack("<I", inferior.read_memory(self.ptr, 4))[0]
            self.raw_memory = bytes(memoryview(inferior.read_memory(self.ptr, self.size)))
            if bson:
                self.is_valid = bson.is_valid(self.raw_memory)

    @staticmethod
    def display_hint():
        """Display hint."""
        return "map"

    def children(self):
        """Children."""
        # Do not decode a BSONObj with an invalid size, or that is considered
        # invalid by pymongo.
        if not bson or not self.is_valid or self.size < 5 or self.size > 17 * 1024 * 1024:
            return

        options = CodecOptions(document_class=collections.OrderedDict)
        bsondoc = bson.decode(self.raw_memory, codec_options=options)

        for key, val in list(bsondoc.items()):
            yield "key", key
            yield "value", bson.json_util.dumps(val)

    def to_string(self):
        """Return BSONObj for printing."""
        # The value has been optimized out.
        if self.size == -1:
            return "BSONObj @ %s - optimized out" % (self.ptr)

        ownership = "owned" if self.val["_ownedBuffer"]["_buffer"]["_holder"]["px"] else "unowned"

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

    def to_string(self):
        """Return OplogEntry for printing."""
        optime = self.val["_entry"]["_opTimeBase"]
        optime_str = "ts(%s, %s)" % (optime["_timestamp"]["secs"], optime["_timestamp"]["i"])
        return "OplogEntry(%s, %s, %s, %s)" % (
            str(self.val["_entry"]["_durableReplOperation"]["_opType"]).split("::")[-1],
            str(self.val["_entry"]["_commandType"]).split("::")[-1],
            self.val["_entry"]["_durableReplOperation"]["_nss"],
            optime_str,
        )


class UUIDPrinter(object):
    """Pretty-printer for mongo::UUID."""

    def __init__(self, val):
        """Initialize UUIDPrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return "string"

    def to_string(self):
        """Return UUID for printing."""
        raw_bytes = [self.val["_uuid"]["_M_elems"][i] for i in range(16)]
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
        return "string"

    def to_string(self):
        """Return OID for printing."""
        raw_bytes = [int(self.val["_data"][i]) for i in range(OBJECT_ID_WIDTH)]
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
        return "string"

    ## Get the address at given offset of data as the selected pointer type
    def __get_data_address(self, ptr, offset):
        ptr_type = gdb.lookup_type(ptr).pointer()
        return self.val["_data"]["_M_elems"][offset].address.cast(ptr_type)

    def to_string(self):
        """Return RecordId for printing."""
        rid_format = int(self.val["_format"])
        if rid_format == 0:
            return "null RecordId"
        elif rid_format == 1:
            koffset = 8 - 1  ##  std::alignment_of_v<int64_t> - sizeof(Format); (see record_id.h)
            rid_address = self.__get_data_address("int64_t", koffset)
            return "RecordId long: %d" % int(rid_address.dereference())
        elif rid_format == 2:
            str_len = self.__get_data_address("int8_t", 0).dereference()
            array_address = self.__get_data_address("int8_t", 1)
            raw_bytes = [array_address[i] for i in range(0, str_len)]
            hex_bytes = [hex(b & 0xFF)[2:].zfill(2) for b in raw_bytes]
            return "RecordId small string %d hex bytes: %s" % (str_len, str("".join(hex_bytes)))
        elif rid_format == 3:
            koffset = (
                8 - 1
            )  ## std::alignment_of_v<ConstSharedBuffer> - sizeof(Format); (see record_id.h)
            buffer = self.__get_data_address("mongo::ConstSharedBuffer", koffset).dereference()
            holder_ptr = holder = buffer["_buffer"]["_holder"]["px"]
            holder = holder.dereference()
            str_len = int(holder["_capacity"])
            # Start of data is immediately after pointer for holder
            start_ptr = (holder_ptr + 1).dereference().cast(lookup_type("char")).address
            raw_bytes = [start_ptr[i] for i in range(0, str_len)]
            hex_bytes = [hex(b & 0xFF)[2:].zfill(2) for b in raw_bytes]
            return "RecordId big string %d hex bytes @ %s: %s" % (
                str_len,
                holder_ptr + 1,
                str("".join(hex_bytes)),
            )
        else:
            return "unknown RecordId format: %d" % rid_format


MAX_DB_NAME_LENGTH = 63
TENANT_ID_MASK = 0x80
OBJECT_ID_WIDTH = 12


def extract_tenant_id(data):
    raw_bytes = [int(data[i]) for i in range(1, OBJECT_ID_WIDTH + 1)]
    return "".join([hex(b & 0xFF)[2:].zfill(2) for b in raw_bytes])


def is_small_string(flags):
    return bool(flags & 0b00000010)


def small_string_size(flags):
    return flags >> 2


class DatabaseNamePrinter(object):
    """Pretty-printer for mongo::DatabaseName."""

    def __init__(self, val):
        """Initialize DatabaseNamePrinter."""
        self.val = val

    @staticmethod
    def display_hint():
        """Display hint."""
        return "string"

    def _get_storage_data(self):
        """Return the data pointer from the _data Storage class."""
        data = self.val["_data"]
        flags = data["_flags"]

        data_ptr = data["_data"]
        if is_small_string(flags):
            return data_ptr.address, small_string_size(flags)
        else:
            return data_ptr, data["_length"]

    def _get_string(self, address, size):
        data = gdb.selected_inferior().read_memory(address, size).tobytes()
        tenant = data[0] & TENANT_ID_MASK

        if tenant:
            return f"{extract_tenant_id(data)}_{data[1 + OBJECT_ID_WIDTH :].decode()}"
        else:
            return data[1:].decode()

    def to_string(self):
        """Return string representation of NamespaceString."""
        address, size = self._get_storage_data()
        # Do not decode the DatabaseName if the parsed size exceeds maximum.
        maxSize = 1 + OBJECT_ID_WIDTH + MAX_DB_NAME_LENGTH
        if size > maxSize:
            return "DatabaseName with size {} exceeds maximum {}, _data = {}".format(
                size, maxSize, self.val["_data"]
            )
        else:
            return self._get_string(address, size)


class DecorablePrinter(object):
    """Pretty-printer for mongo::Decorable<>."""

    def __init__(self, val):
        """Initialize DecorablePrinter."""
        self.val = val
        self.start, self.count = get_decorable_info(val)

    @staticmethod
    def display_hint():
        """Display hint."""
        return "map"

    def to_string(self):
        """Return Decorable for printing."""
        return "Decorable<{}> with {} elems ".format(self.val.type.template_argument(0), self.count)

    def children(self):
        """Children."""
        for index in range(self.count):
            try:
                deco_type_name, obj, obj_addr = get_object_decoration(self.val, self.start, index)
                yield ("key", "{}:{}:{}".format(index, obj_addr, deco_type_name))
                yield ("value", obj)
            except Exception as err:
                print("Failed to look up decoration type: " + deco_type_name + ": " + str(err))


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
        with open("./src/third_party/wiredtiger/src/include/wiredtiger.h.in") as wiredtiger_header:
            file_contents = wiredtiger_header.read()
            cursor_flags_re = re.compile(r"#define\s+WT_CURSTD_(\w+)\s+0x(\d+)u")
            cursor_flags = cursor_flags_re.findall(file_contents)[::-1]
    except IOError:
        cursor_flags = []

    def __init__(self, val):
        """Initializer."""
        self.val = val

    def to_string(self):
        """to_string."""
        return None

    def children(self):
        """children."""
        for field in self.val.type.fields():
            field_val = self.val[field.name]
            if field.name == "flags":
                yield (
                    "flags",
                    "{} ({})".format(field_val, str(_get_flags(field_val, self.cursor_flags))),
                )
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

    def to_string(self):
        """to_string."""
        return None

    def children(self):
        """children."""
        for field in self.val.type.fields():
            field_val = self.val[field.name]
            if field.name == "flags":
                yield (
                    "flags",
                    "{} ({})".format(field_val, str(_get_flags(field_val, self.session_flags))),
                )
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

    def to_string(self):
        """to_string."""
        return None

    def children(self):
        """children."""
        for field in self.val.type.fields():
            field_val = self.val[field.name]
            if field.name == "flags":
                yield (
                    "flags",
                    "{} ({})".format(field_val, str(_get_flags(field_val, self.txn_flags))),
                )
            else:
                yield (field.name, field_val)


def absl_insert_version_after_absl(cpp_name):
    """Insert version inline namespace after the first `absl` namespace found in the given string."""
    # See more:
    # https://github.com/abseil/abseil-cpp/blob/929c17cf481222c35ff1652498994871120e832a/absl/base/options.h#L203
    ABSL_OPTION_INLINE_NAMESPACE_NAME = "lts_20250512"

    absl_ns_str = "absl::"
    absl_ns_start = cpp_name.find(absl_ns_str)
    if absl_ns_start == -1:
        raise ValueError("No `absl` namespace found in " + cpp_name)

    absl_ns_end = absl_ns_start + len(absl_ns_str)

    return (
        cpp_name[:absl_ns_end] + ABSL_OPTION_INLINE_NAMESPACE_NAME + "::" + cpp_name[absl_ns_end:]
    )


def absl_get_settings(val):
    """Gets the settings_ field for abseil (flat/node)_hash_(map/set)."""
    try:
        common_fields_storage_type = gdb.lookup_type(
            absl_insert_version_after_absl(
                "absl::container_internal::internal_compressed_tuple::Storage"
            )
            + absl_insert_version_after_absl("<absl::container_internal::CommonFields, 0, false>")
        )
    except gdb.error as err:
        if not err.args[0].startswith("No type named "):
            raise

        # Abseil uses `inline namespace lts_20250512 { ... }` for its container types. This
        # can inhibit GDB from resolving type names when the inline namespace appears within
        # a template argument.
        common_fields_storage_type = gdb.lookup_type(
            absl_insert_version_after_absl(
                "absl::container_internal::internal_compressed_tuple::Storage"
                "<absl::container_internal::CommonFields, 0, false>"
            )
        )

    # The Hash, Eq, or Alloc functors may not be zero-sized objects.
    # mongo::LogicalSessionIdHash is one such example. An explicit cast is needed to
    # disambiguate which `value` member variable of the CompressedTuple is to be accessed.
    return val["settings_"].cast(common_fields_storage_type)["value"]


def absl_container_size(settings):
    return settings["size_"]["data_"] >> 17


def absl_get_nodes(val):
    """Return a generator of every node in absl::container_internal::raw_hash_set and derived classes."""
    settings = absl_get_settings(val)

    size = absl_container_size(settings)
    if size == 0:
        return

    capacity = int(settings["capacity_"])
    heap = settings["heap_or_soo_"]["heap"]
    ctrl = heap["control"]

    # Derive the underlying type stored in the container.
    slot_type = lookup_type(str(val.type.strip_typedefs()) + "::slot_type").strip_typedefs()

    # Using the array of ctrl bytes, search for in-use slots and return them
    # https://github.com/abseil/abseil-cpp/blob/8a3caf7dea955b513a6c1b572a2423c6b4213402/absl/container/internal/raw_hash_set.h#L2108-L2113
    for item in range(capacity):
        ctrl_t = int(ctrl[item])
        if ctrl_t >= 0:
            yield heap["slot_array"]["p"].cast(slot_type.pointer())[item]


class AbslHashSetPrinterBase(object):
    """Pretty-printer base class for absl::[node/flat]_hash_set<>."""

    def __init__(self, val, to_str):
        """Initialize absl::[node/flat]_hash_set."""
        self.val = val
        self.to_str = to_str

    @staticmethod
    def display_hint():
        """Display hint."""
        return "array"

    def to_string(self):
        """Return absl::[node/flat]_hash_set for printing."""
        return "absl::%s_hash_set<%s> with %s elems " % (
            self.to_str,
            self.val.type.template_argument(0),
            absl_container_size(absl_get_settings(self.val)),
        )


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
        return "map"

    def to_string(self):
        """Return absl::[node/flat]_hash_map for printing."""
        return "absl::%s_hash_map<%s, %s> with %s elems " % (
            self.to_str,
            self.val.type.template_argument(0),
            self.val.type.template_argument(1),
            absl_container_size(absl_get_settings(self.val)),
        )


class AbslNodeHashMapPrinter(AbslHashMapPrinterBase):
    """Pretty-printer for absl::node_hash_map<>."""

    def __init__(self, val):
        """Initialize absl::node_hash_map."""
        AbslHashMapPrinterBase.__init__(self, val, "node")

    def children(self):
        """Children."""
        for kvp in absl_get_nodes(self.val):
            yield ("key", kvp["first"])
            yield ("value", kvp["second"])


class AbslFlatHashMapPrinter(AbslHashMapPrinterBase):
    """Pretty-printer for absl::flat_hash_map<>."""

    def __init__(self, val):
        """Initialize absl::flat_hash_map."""
        AbslHashMapPrinterBase.__init__(self, val, "flat")

    def children(self):
        """Children."""
        for kvp in absl_get_nodes(self.val):
            yield ("key", kvp["key"])
            yield ("value", kvp["value"]["second"])


class ImmutableMapIter(ImmerListIter):
    def __init__(self, val):
        super().__init__(val)
        self.max = (1 << 64) - 1
        self.pair = None
        self.curr = (None, self.max, self.max)

    def __next__(self):
        if self.pair:
            result = ("value", self.pair["second"])
            self.pair = None
            self.i += 1
            return result
        if self.i == self.size:
            raise StopIteration
        if self.i < self.curr[1] or self.i >= self.curr[2]:
            self.curr = self.region()
        self.pair = self.curr[0][self.i - self.curr[1]].cast(
            gdb.lookup_type(self.v.type.template_argument(0).name)
        )
        result = ("key", self.pair["first"])
        return result


class ImmutableMapPrinter:
    """Pretty-printer for mongo::immutable::map<>."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "%s of size %d" % (self.val.type, int(self.val["_storage"]["impl_"]["size"]))

    def children(self):
        return ImmutableMapIter(self.val["_storage"])

    def display_hint(self):
        return "map"


class ImmutableSetPrinter:
    """Pretty-printer for mongo::immutable::set<>."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "%s of size %d" % (self.val.type, int(self.val["_storage"]["impl_"]["size"]))

    def children(self):
        return ImmerListIter(self.val["_storage"])

    def display_hint(self):
        return "array"


def find_match_brackets(search, opening="<", closing=">"):
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
                if index + 1 == len(lookup_tag) and lookup_tag.find(printer.prefix) == 0:
                    return printer.printer(val)
            elif lookup_tag == printer.prefix:
                return printer.printer(val)

        return None


class WtUpdateToBsonPrinter(object):
    """Pretty printer for WT_UPDATE. Interpreting the `data` field as bson."""

    def __init__(self, val):
        """Initializer."""
        self.val = val
        self.size = self.val["size"]
        self.ptr = self.val["data"]

    @staticmethod
    def display_hint():
        """DisplayHint."""
        return "map"

    def to_string(self):
        """ToString."""
        elems = []
        for idx in range(len(self.val.type.fields())):
            fld = self.val.type.fields()[idx]
            val = self.val[fld.name]
            elems.append(str((fld.name, str(val))))
        return "WT_UPDATE: \n  %s" % ("\n  ".join(elems))

    def children(self):
        """children."""
        if self.val["type"] != 3:
            # Type 3 is a "normal" update. Notably type 4 is a deletion and type 1 represents a
            # delta relative to the previous committed version in the update chain. Only attempt
            # to parse type 3 as bson.
            return

        memory = gdb.selected_inferior().read_memory(self.ptr, self.size).tobytes()
        bsonobj = None
        try:
            bsonobj = next(bson.decode_iter(memory))
        except bson.errors.InvalidBSON:
            return

        for key, value in list(bsonobj.items()):
            yield "key", key
            yield "value", bson.json_util.dumps(value)


def make_inverse_enum_dict(enum_type_name):
    """
    Create a dictionary that maps enum values to the unqualified names of the enum elements.

    For example, if the enum type is 'mongo::sbe::vm::Builtin' with an element 'regexMatch', the
    dictionary will contain 'regexMatch' value and not 'mongo::sbe::vm::Builtin::regexMatch'.
    """
    enum_dict = gdb.types.make_enum_dict(lookup_type(enum_type_name))
    enum_inverse_dic = dict()
    for key, value in enum_dict.items():
        enum_inverse_dic[int(value)] = key.split("::")[-1]  # take last element
    return enum_inverse_dic


def read_as_integer(pmem, size):
    """Read 'size' bytes at 'pmem' as an integer."""
    # We assume the same platform for the debugger and the debuggee (thus, 'sys.byteorder'). If
    # this becomes a problem look into whether it's possible to determine the byteorder of the
    # inferior.
    return int.from_bytes(
        gdb.selected_inferior().read_memory(pmem, size).tobytes(),
        sys.byteorder,
    )


def read_as_integer_signed(pmem, size):
    """Read 'size' bytes at 'pmem' as an integer."""
    # We assume the same platform for the debugger and the debuggee (thus, 'sys.byteorder'). If
    # this becomes a problem look into whether it's possible to determine the byteorder of the
    # inferior.
    return int.from_bytes(
        gdb.selected_inferior().read_memory(pmem, size).tobytes(),
        sys.byteorder,
        signed=True,
    )


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
        storage = self.val["_instrs"]["storage_"]
        meta = storage["metadata_"].cast(lookup_type("size_t"))
        self.is_inlined = meta % 2 == 0
        self.size = meta >> 1
        self.pdata = (
            storage["data_"]["inlined"]["inlined_data"].cast(lookup_type("uint8_t").pointer())
            if self.is_inlined
            else storage["data_"]["allocated"]["allocated_data"]
        )

        # Precompute lookup tables for Instructions and Builtins.
        self.optags_lookup = make_inverse_enum_dict("mongo::sbe::vm::Instruction::Tags")
        self.builtins_lookup = make_inverse_enum_dict("mongo::sbe::vm::Builtin")
        self.valuetags_lookup = make_inverse_enum_dict("mongo::sbe::value::TypeTags")

    def to_string(self):
        """Return sbe::vm::CodeFragment for printing."""
        return "%s" % (self.val.type)

    def children(self):
        """children."""
        yield "_instrs", '{... (to see raw output, run "disable pretty-printer")}'
        yield "_fixUps", self.val["_fixUps"]
        yield "_stackSize", self.val["_stackSize"]

        yield "inlined", self.is_inlined
        yield "instrs data at", "[{} - {}]".format(hex(self.pdata), hex(self.pdata + self.size))
        yield "instrs total size", self.size

        # Sizes for types we'll use when parsing the insructions stream.
        int_size = lookup_type("int").sizeof
        ptr_size = lookup_type("void").pointer().sizeof
        tag_size = lookup_type("mongo::sbe::value::TypeTags").sizeof
        value_size = lookup_type("mongo::sbe::value::Value").sizeof
        uint8_size = lookup_type("uint8_t").sizeof
        uint32_size = lookup_type("uint32_t").sizeof
        uint64_size = lookup_type("uint64_t").sizeof
        builtin_size = lookup_type("mongo::sbe::vm::Builtin").sizeof
        time_unit_size = lookup_type("mongo::TimeUnit").sizeof
        timezone_size = lookup_type("mongo::TimeZone").sizeof
        day_of_week_size = lookup_type("mongo::DayOfWeek").sizeof

        cur_op = self.pdata
        end_op = self.pdata + self.size
        instr_count = 0
        error = False
        while cur_op < end_op:
            op_addr = cur_op
            op_tag = read_as_integer(op_addr, 1)

            if op_tag not in self.optags_lookup:
                yield hex(op_addr), "unknown op tag: {}".format(op_tag)
                error = True
                break
            op_name = self.optags_lookup[op_tag]

            cur_op += 1
            instr_count += 1

            # Some instructions have extra arguments, embedded into the ops stream.
            args = ""
            if op_name in [
                "pushLocalVal",
                "pushMoveLocalVal",
                "pushOneArgLambda",
                "pushTwoArgLambda",
            ]:
                args = "arg: " + str(read_as_integer(cur_op, int_size))
                cur_op += int_size
            elif op_name in ["jmp", "jmpTrue", "jmpFalse", "jmpNothing", "jmpNotNothing"]:
                offset = read_as_integer_signed(cur_op, int_size)
                cur_op += int_size
                args = "offset: " + str(offset) + ", target: " + hex(cur_op + offset)
            elif op_name in ["pushConstVal", "getFieldImm"]:
                tag = read_as_integer(cur_op, tag_size)
                args = (
                    "tag: "
                    + self.valuetags_lookup.get(tag, "unknown")
                    + ", value: "
                    + hex(read_as_integer(cur_op + tag_size, value_size))
                )
                cur_op += tag_size + value_size
            elif op_name in ["pushAccessVal", "pushMoveVal"]:
                args = "accessor: " + hex(read_as_integer(cur_op, ptr_size))
                cur_op += ptr_size
            elif op_name in ["numConvert"]:
                args = "convert to: " + self.valuetags_lookup.get(
                    read_as_integer(cur_op, tag_size), "unknown"
                )
                cur_op += tag_size
            elif op_name in ["typeMatchImm"]:
                args = "mask: " + hex(read_as_integer(cur_op, uint32_size))
                cur_op += uint32_size
            elif op_name in ["function", "functionSmall"]:
                arity_size = (
                    lookup_type("mongo::sbe::vm::ArityType").sizeof
                    if op_name == "function"
                    else lookup_type("mongo::sbe::vm::SmallArityType").sizeof
                )
                builtin_id = read_as_integer(cur_op, builtin_size)
                args = "builtin: " + self.builtins_lookup.get(builtin_id, "unknown")
                args += " arity: " + str(read_as_integer(cur_op + builtin_size, arity_size))
                cur_op += builtin_size + arity_size
            elif op_name in ["fillEmptyImm"]:
                args = "Instruction::Constants: " + str(read_as_integer(cur_op, uint8_size))
                cur_op += uint8_size
            elif op_name in ["traverseFImm", "traversePImm"]:
                position = read_as_integer(cur_op, uint8_size)
                cur_op += uint8_size
                const_enum = read_as_integer(cur_op, uint8_size)
                cur_op += uint8_size
                args = (
                    "providePosition: "
                    + str(position)
                    + ", Instruction::Constants: "
                    + str(const_enum)
                    + ", offset: "
                    + str(read_as_integer_signed(cur_op, int_size))
                )
                cur_op += int_size
            elif op_name in ["dateTruncImm"]:
                unit = read_as_integer(cur_op, time_unit_size)
                cur_op += time_unit_size
                args = "unit: " + str(unit)
                bin_size = read_as_integer(cur_op, uint64_size)
                cur_op += uint64_size
                args += ", binSize: " + str(bin_size)
                timezone = read_as_integer(cur_op, timezone_size)
                cur_op += timezone_size
                args += ", timezone: " + hex(timezone)
                day_of_week = read_as_integer(cur_op, day_of_week_size)
                cur_op += day_of_week_size
                args += ", dayOfWeek: " + str(day_of_week)
            elif op_name in ["traverseCsiCellValues", "traverseCsiCellTypes"]:
                offset = read_as_integer_signed(cur_op, int_size)
                cur_op += int_size
                args = "lambda at: " + hex(cur_op + offset)

            yield hex(op_addr), "{} ({})".format(op_name, args)

        yield (
            "instructions count",
            instr_count if not error else "? (successfully parsed {})".format(instr_count),
        )


def build_pretty_printer():
    """Build a pretty printer."""
    pp = MongoPrettyPrinterCollection()
    pp.add("BSONObj", "mongo::BSONObj", False, BSONObjPrinter)
    pp.add("DatabaseName", "mongo::DatabaseName", False, DatabaseNamePrinter)
    pp.add("NamespaceString", "mongo::NamespaceString", False, DatabaseNamePrinter)
    pp.add("Decorable", "mongo::Decorable", True, DecorablePrinter)
    pp.add("Status", "mongo::Status", False, StatusPrinter)
    pp.add("StatusWith", "mongo::StatusWith", True, StatusWithPrinter)
    pp.add("StringData", "mongo::StringData", False, StringDataPrinter)
    pp.add(
        "node_hash_map",
        absl_insert_version_after_absl("absl::node_hash_map"),
        True,
        AbslNodeHashMapPrinter,
    )
    pp.add(
        "node_hash_set",
        absl_insert_version_after_absl("absl::node_hash_set"),
        True,
        AbslNodeHashSetPrinter,
    )
    pp.add(
        "flat_hash_map",
        absl_insert_version_after_absl("absl::flat_hash_map"),
        True,
        AbslFlatHashMapPrinter,
    )
    pp.add(
        "flat_hash_set",
        absl_insert_version_after_absl("absl::flat_hash_set"),
        True,
        AbslFlatHashSetPrinter,
    )
    pp.add("RecordId", "mongo::RecordId", False, RecordIdPrinter)
    pp.add("UUID", "mongo::UUID", False, UUIDPrinter)
    pp.add("OID", "mongo::OID", False, OIDPrinter)
    pp.add("OplogEntry", "mongo::repl::OplogEntry", False, OplogEntryPrinter)
    pp.add("__wt_cursor", "__wt_cursor", False, WtCursorPrinter)
    pp.add("__wt_session_impl", "__wt_session_impl", False, WtSessionImplPrinter)
    pp.add("__wt_txn", "__wt_txn", False, WtTxnPrinter)
    pp.add("__wt_update", "__wt_update", False, WtUpdateToBsonPrinter)
    pp.add("CodeFragment", "mongo::sbe::vm::CodeFragment", False, SbeCodeFragmentPrinter)
    pp.add("boost::optional", "boost::optional", True, BoostOptionalPrinter)
    pp.add("immutable::map", "mongo::immutable::map", True, ImmutableMapPrinter)
    pp.add("immutable::set", "mongo::immutable::set", True, ImmutableSetPrinter)

    # Optimizer/ABT related pretty printers that can be used only with a running process.
    register_optimizer_printers(pp)

    return pp


###################################################################################################
#
# Setup
#
###################################################################################################

# Register pretty-printers, replace existing mongo printers
gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer(), True)

print("MongoDB GDB pretty-printers loaded")

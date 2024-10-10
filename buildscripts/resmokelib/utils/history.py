"""Contains classes and methods for tracking historic state."""

import copy
import traceback
import typing
from abc import ABC, abstractmethod
from collections import defaultdict
from collections.abc import MutableMapping
from dataclasses import asdict, dataclass, field
from enum import Enum

import yaml

from buildscripts.resmokelib.utils import default_if_none, load_yaml, load_yaml_file, registry

# How large of a stack to take for each location.
STACK_LIMIT = 3

_HISTORICS = {}  # type: ignore

SCHEMA_VERSION = "0.1"


def make_historic(obj):
    """Convert a python object into a corresponding Historic."""
    if isinstance(obj, ALLOWED_TYPES):
        return obj
    obj_class = _HISTORICS[type(obj).__name__]
    return obj_class.from_python_obj(obj)


def historic_from_stored_dict(stored_dict):
    """Convert a dict for storage into a historic object."""
    obj_class = _HISTORICS[stored_dict["object_class"]]
    return obj_class.from_storable_dict(stored_dict)


def storable_dict_from_historic(to_storable):
    """Convert a historic into a dict that can be stored in history."""
    if isinstance(to_storable, Historic):
        return to_storable.to_storable_dict()
    else:
        # If we're a primitive immutable
        return to_storable


class Historic(ABC, metaclass=registry.make_registry_metaclass(_HISTORICS, type(ABC))):  # pylint: disable=invalid-metaclass
    """ABC for classes that have trackable historic state."""

    def __init__(self):
        """Initialize subscriber list."""
        self._subscribers = []

    def subscribe(self, subscriber, key):
        """
        Subscribe to the Historic object.

        The subscriber's accept_write is called on an update.
        """
        if not isinstance(subscriber, Historic):
            raise ValueError("Subscribers should inherit from the Historic ABC.")

        self._subscribers.append(Subscriber(obj=subscriber, key=key))

    def unsubscribe(self, subscriber):
        """Allow a subscriber to unsubscribe from notifications."""
        self._subscribers = [sub for sub in self._subscribers if sub.obj is not subscriber]

    def notify_subscriber_write(self):
        """Notify the subscribers that a write has happened."""
        for subscriber in self._subscribers:
            subscriber.obj.accept_write(subscriber.key)

    @abstractmethod
    def to_storable_dict(self):
        """
        Convert this object to a dict that can be stored in yaml.

        Note that if a Historic stores history data itself, this is allowed to
        be lost in the storage/retrieval of subordinate Historics.
        """
        return

    @staticmethod
    @abstractmethod
    def from_storable_dict(raw_dict):
        """Create a new object from the raw dict returned by to_storable_dict."""
        return

    @staticmethod
    @abstractmethod
    def from_python_obj(obj):
        """
        Create a new Historic from the given python object.

        If inheriting from this in a class that wraps a python object,
        include a REGISTERED_NAME string attribute for the type it converts from.
        Otherwise, override this function to just return obj.
        """
        return

    def accept_write(self, key):  # pylint: disable=unused-argument
        """
        Update state based on a subscriber's write.

        Override this method if a class also tracks historic state.
        """
        self.notify_subscriber_write()


@dataclass
class Subscriber:
    """Class representing the subscriber to a Historic."""

    obj: "typing.Any"
    key: "typing.Any"


# 1. We only allow immutable types or types that have special logic
#    for being inside a HistoryDict, or else we may miss changes and
#    have difficulty converting to yaml.
# 2. Dictionaries are allowed to be passed in but are implicitly converted to Historic.
ALLOWED_TYPES = (bool, int, float, str, type(None), Historic)


class HistoryDict(MutableMapping, Historic):  # pylint: disable=too-many-ancestors
    """
    Dict-like class that tracks history.

    Smart stored classes can decide for themselves what
    counts as an access. Don't assume thread safety.
    Note that this class will deep-copy stored values.
    """

    REGISTERED_NAME = "dict"

    def __init__(self, filename=None, yaml_string=None, raw_dict=None):
        """Init from a yaml file, from a yaml string, or default-construct."""

        super(HistoryDict, self).__init__()

        if filename is not None and yaml_string is not None:
            raise ValueError("Cannot construct HistoryDict from both yaml object and file.")

        self._history_store = defaultdict(list)
        self._value_store = dict()
        self._global_time = 0

        raw_dict = default_if_none(raw_dict, {})
        if filename is not None:
            raw_dict = load_yaml_file(filename)
        elif yaml_string is not None:
            raw_dict = load_yaml(yaml_string)
        else:
            return  # Just default-construct.

        schema_version = raw_dict["SchemaVersion"]
        if schema_version != SCHEMA_VERSION:
            raise ValueError(
                f"Invalid schema version. Expected {SCHEMA_VERSION} but found {schema_version}."
            )
        history_dict = raw_dict["History"]
        for key in history_dict:
            for raw_access in history_dict[key]:
                access = Access.from_dict(raw_access)
                self._history_store[key].append(access)
                self._global_time = max(access.time, self._global_time)
            last_val = self._retrieve_last_value(key)
            if last_val is not TOMBSTONE:
                self._value_store[key] = last_val

        # The next recorded global time should be 1 higher than the last.
        self._global_time += 1

    def dump_history(self, filename=None, include_location=False):
        """Dump the history to as yaml."""

        # We can't safe_dump python objects, so we convert the whole store
        # to yaml-able data. (We assume the stored contents are yaml-able.)
        dumpable = dict()
        for key in self._history_store:
            dumpable[key] = list()
            for access in self._history_store[key]:
                access_dict = access.as_dict()
                if include_location:
                    access_dict["location"] = PipeLiteral(access_dict["location"])
                else:
                    del access_dict["location"]
                dumpable[key].append(access_dict)

        to_dump = {"History": dumpable}
        dump = yaml.dump(to_dump)

        # Improves human readability for this data a ton.
        processed = []
        for line in dump.splitlines():
            if not line.startswith(("  -", "    ")):
                # If this line starts a top-level key.
                processed.append("\n")
            processed.append(line)
        output = "\n".join(processed)

        # Make sure SchemaVersion is at the top.
        output = f'SchemaVersion: "{SCHEMA_VERSION}"\n' + output
        if filename is not None:
            with open(filename, "w") as fp:
                fp.write(output)

        return output

    def write_equals(self, other_dict):
        """Compare two dicts for write equality."""
        if not len(other_dict._value_store) == len(self._value_store):  # pylint: disable=protected-access
            return False

        for key in self._value_store:
            our_writes = [
                access.value_written
                for access in self._history_store[key]
                if access.type == AccessType.WRITE
            ]
            their_writes = [
                access.value_written
                for access in other_dict._history_store[key]  # pylint: disable=protected-access
                if access.type == AccessType.WRITE
            ]
            if not our_writes == their_writes:
                return False
        return True

    def to_storable_dict(self):
        """Convert to a dict for storage, overrides Historic."""
        storable_dict = {}
        for key, value in self._value_store.items():
            storable_dict[key] = storable_dict_from_historic(value)
        return {"object_class": HistoryDict.REGISTERED_NAME, "object_value": storable_dict}

    @staticmethod
    def from_storable_dict(raw_dict):
        """Convert from a dict for storage, overrides Historic."""
        return HistoryDict(raw_dict=raw_dict["object_value"])

    @staticmethod
    def from_python_obj(obj):
        """Convert from a python object, overrides Historic."""
        if not isinstance(obj, dict):
            raise ValueError("HistoryDict can only be converted from dict python objects.")
        history_dict = HistoryDict()
        for key, value in obj.items():
            history_dict[key] = make_historic(value)
        return history_dict

    def accept_write(self, key):
        """Record subscribee's write. Overrides Historic."""
        self._record_write(key, self._value_store[key])
        super(HistoryDict, self).accept_write(key)

    def copy(self):
        """
        Shallow-copy the value store, deep-copy history.

        Don't record writes here.
        """
        history_dict = HistoryDict()
        history_dict._global_time = self._global_time  # pylint: disable=protected-access
        history_dict._history_store = copy.deepcopy(self._history_store)  # pylint: disable=protected-access
        for key, value in self.items():
            history_dict[key] = make_historic(value)
        return history_dict

    def __getitem__(self, key):
        # We don't return a deep copy because we rely on objects to alert us
        # when modified.
        return self._value_store[key]

    def __setitem__(self, key, value):
        # Implicitly convert dictionaries to HistoricDicts to avoid users having to manually wrap dictionaries.
        # This should only used when assigning a dictionary directly to a key in HistoryDict and not a standalone
        # variable, as its history will not be recorded.
        if isinstance(value, dict):
            value = make_historic(value)

        if not isinstance(value, ALLOWED_TYPES):
            raise ValueError(
                f"HistoryDict cannot store type {type(value)}."
                " Please use a different type or create a Historic wrapper."
            )
        self._record_write(key, value)
        self._value_store[key] = value
        if isinstance(value, HistoryDict):
            value.subscribe(self, key)

        self.notify_subscriber_write()

    def __delitem__(self, key):
        self._record_delete(key)
        if isinstance(self._value_store[key], Historic):
            self._value_store[key].unsubscribe(self)
        del self._value_store[key]

        self.notify_subscriber_write()

    def __iter__(self):
        return iter(self._value_store)

    def __len__(self):
        return len(self._value_store)

    def __str__(self):
        # Dict's str doesn't recursively convert subordinate HistoryDicts
        pairs = []
        for key, value in self._value_store.items():
            if isinstance(value, str):
                pairs.append(f"'{key}': '{str(value)}'")
            else:
                pairs.append(f"'{key}': {str(value)}")
        return "{" + ", ".join(pairs) + "}"

    def __repr__(self):
        # eval(repr(self)) isn't valid, but this is at least useful for debugging.
        return f"{self.__class__.__name__}({repr(self._value_store)})"

    def _record_write(self, key, value):
        written = None
        if type(value) in ALLOWED_TYPES and value is not Historic:
            written = value

        cur_access = Access(
            type=AccessType.WRITE,
            location=_get_location(),
            value_written=written,
            time=self._global_time,
        )
        self._history_store[key].append(cur_access)
        self._global_time += 1

    def _record_delete(self, key):
        cur_access = Access(
            type=AccessType.DELETE,
            location=_get_location(),
            value_written=None,
            time=self._global_time,
        )
        self._history_store[key].append(cur_access)
        self._global_time += 1

    def _retrieve_last_value(self, key):
        for access in reversed(self._history_store[key]):
            if access.type == AccessType.WRITE:
                if isinstance(access.value_written, dict):
                    return historic_from_stored_dict(access.value_written)
                else:
                    return copy.deepcopy(access.value_written)
            elif access.type == AccessType.DELETE:
                break
        return TOMBSTONE


# Represents a value that was deleted (or was never created).
TOMBSTONE = object()


class AccessType(Enum):
    """Class representing the operation performed in an accesss."""

    READ = 0  # Reads are not recorded here.
    WRITE = 1
    DELETE = 2


@dataclass
class Access:
    """Class representing an access to store in the dict's history."""

    type: "AccessType"
    time: int
    location: ["traceback.FrameSummary"] = field(default_factory=list)
    value_written: "typing.Any" = None

    def as_dict(self):
        """Convert this class into a dict (accounting for AccessType)."""
        self_dict = asdict(self)
        self_dict["type"] = self_dict["type"].name
        self_dict["value_written"] = copy.deepcopy(self.value_written)
        return self_dict

    @staticmethod
    def from_dict(raw_dict):
        """Retrieve this class from a dict (accounting for AccessType)."""
        return Access(
            type=AccessType[raw_dict["type"]],
            time=raw_dict["time"],
            location=raw_dict["location"] if "location" in raw_dict else list(),
            value_written=copy.deepcopy(raw_dict["value_written"])
            if "value_written" in raw_dict
            else None,
        )


def _get_location():
    """Return the location as a string, accounting for this function and the parent in the stack."""
    return "".join(traceback.format_stack(limit=STACK_LIMIT + 2)[:-2])


class PipeLiteral(str):
    """Construct with a string to create a pipe literal for yaml representation."""

    pass


def pipe_literal_representer(dumper, data):
    """Create a representer for pipe literals, used internally for pyyaml."""
    return dumper.represent_scalar("tag:yaml.org,2002:str", data, style="|")


yaml.add_representer(PipeLiteral, pipe_literal_representer)

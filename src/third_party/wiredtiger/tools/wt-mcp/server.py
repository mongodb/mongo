#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

"""
WiredTiger MCP Server

A Model Context Protocol (MCP) server with persistent connection management for
WiredTiger databases. Connections, sessions, cursors, and transactions are managed
explicitly through tools, allowing stateful debugging workflows across multiple
tool calls.
"""
import argparse
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from dotenv import load_dotenv
import json
import os
from pydantic import Field as PydanticField
import sys
from typing import Dict, List, Optional

# Import MCP server library
from mcp.server.fastmcp import FastMCP, Context
from mcp.server.fastmcp.utilities.logging import configure_logging, get_logger

load_dotenv()
build_dir = os.getenv("WT_BUILDDIR")
if not build_dir:
    # Derive project root from this script's location: tools/wt-mcp/server.py -> project root
    project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    for candidate in ["build", "cmake_build"]:
        candidate_path = os.path.join(project_root, candidate)
        if os.path.isdir(candidate_path):
            build_dir = candidate_path
            break
    if not build_dir:
        print("Error: WT_BUILDDIR environment variable not set and no build directory found. "
              "Looked for 'build' and 'cmake_build' in the project root. "
              "Please set WT_BUILDDIR to the WiredTiger build directory.")
        sys.exit(1)

# Add the build directory to sys.path
sys.path.insert(0, build_dir)
# Add the lang/python directory to sys.path
lang_python_dir = os.path.join(build_dir, "lang", "python")
sys.path.insert(0, lang_python_dir)
# Add the tools directory to sys.path
tools_dir = os.path.join(build_dir, "..", "tools")
sys.path.insert(0, tools_dir)

try:
    import wiredtiger
except ImportError:
    print("Error: WiredTiger Python API not found. Please ensure it is built and available in the Python path.")
    sys.exit(1)

logger = get_logger(__name__)

# ---------------------------------------------------------------------------
# Metadata config parsing
# ---------------------------------------------------------------------------

def _parse_config_field(config_str: str, key: str) -> Optional[str]:
    """
    Extract a top-level configuration field from a WiredTiger config string.

    Handles nested parentheses correctly --only splits on commas that are not
    inside parenthesised groups.  Returns None if the key is not found.
    """
    depth = 0
    start = 0
    parts: list[str] = []

    for i, ch in enumerate(config_str):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch == ',' and depth == 0:
            parts.append(config_str[start:i])
            start = i + 1
    parts.append(config_str[start:])

    for part in parts:
        if "=" in part:
            k, v = part.split("=", 1)
            if k.strip() == key:
                return v.strip()
    return None

# ---------------------------------------------------------------------------
# State management
# ---------------------------------------------------------------------------

@dataclass
class ConnectionInfo:
    conn: wiredtiger.Connection
    session: wiredtiger.Session
    home: str
    config: str

@dataclass
class CursorInfo:
    cursor: wiredtiger.Cursor
    connection_name: str
    uri: str
    key_format: str = ""
    value_format: str = ""

_connections: dict[str, ConnectionInfo] = {}
_cursors: dict[str, CursorInfo] = {}
_cursor_counter: int = 0


def _get_connection(name: str) -> ConnectionInfo:
    if name not in _connections:
        raise ValueError(f"No connection named '{name}'. Use list_connections to see open connections.")
    return _connections[name]


def _get_cursor(name: str) -> CursorInfo:
    if name not in _cursors:
        raise ValueError(f"No cursor named '{name}'.")
    return _cursors[name]


def _close_all():
    for cursor_info in _cursors.values():
        try:
            cursor_info.cursor.close()
        except Exception:
            pass
    _cursors.clear()
    for conn_info in _connections.values():
        try:
            conn_info.session.close()
        except Exception:
            pass
        try:
            conn_info.conn.close()
        except Exception:
            pass
    _connections.clear()


def metadata_search(session, uri):
    """
    Look up a URI in the WiredTiger metadata and return its value.

    Returns the metadata value string if found, or None if the URI is not present.
    """
    cursor = session.open_cursor("metadata:")
    try:
        cursor.set_key(uri)
        if cursor.search() == 0:
            return cursor.get_value()
        return None
    finally:
        cursor.close()


def _convert_key(key_format: str, key_str: str):
    """Convert a string key to the appropriate type using the cached key_format."""
    if not key_format or len(key_format) != 1:
        return key_str

    fmt = key_format[0]
    if fmt in 'bBhHiIlLqQt':
        return int(key_str)
    elif fmt == 'u':
        return bytes(key_str, 'utf-8')
    return key_str


def _convert_value(value_format: str, value_str: str):
    """Convert a string value to the appropriate type using the cached value_format."""
    if not value_format or len(value_format) != 1:
        return value_str

    fmt = value_format[0]
    if fmt in 'bBhHiIlLqQt':
        return int(value_str)
    elif fmt == 'u':
        return bytes(value_str, 'utf-8')
    return value_str


# ---------------------------------------------------------------------------
# Server lifecycle
# ---------------------------------------------------------------------------

@asynccontextmanager
async def server_lifespan(app):
    yield
    _close_all()

mcp = FastMCP(
    name="WiredTiger Tools",
    log_level="INFO",
    debug=True,
    lifespan=server_lifespan,
)

# Argument descriptions
URI_DESCRIPTION = "The URI of the file or table, e.g. 'file:myfile.wt' or 'table:mytable'."
CONN_DESCRIPTION = "The name of an open connection (from open_connection)."
CURSOR_DESCRIPTION = "The name of an open cursor (from open_cursor)."
HOME_DESCRIPTION = "The WiredTiger home directory where the database files are stored."

# ---------------------------------------------------------------------------
# Connection lifecycle tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def open_connection(
    ctx: Context,
    name: str = PydanticField(description="A unique name for this connection, used to reference it in other tools."),
    home: str = PydanticField(description=HOME_DESCRIPTION),
    config: Optional[str] = PydanticField(default=None, description="WiredTiger connection configuration string, e.g. 'statistics=(all)'.")
) -> Dict:
    """
    Open a persistent WiredTiger connection and store it by name.

    A single session is automatically created for the connection. The connection
    remains open until close_connection is called, allowing multiple tool calls
    to reuse it.

    When to use this tool:
        - At the start of a debugging session to open a database
        - When you need to connect to a new WiredTiger home directory
    """
    try:
        if name in _connections:
            return {
                "content": [{
                    "type": "text",
                    "text": f"Error: connection '{name}' already exists. Close it first or choose a different name."
                }]
            }

        await ctx.info(f"Opening connection '{name}' to {home}")
        conn = wiredtiger.wiredtiger_open(home, config or "")
        session = conn.open_session()
        _connections[name] = ConnectionInfo(conn=conn, session=session, home=home, config=config or "")

        return {
            "content": [{
                "type": "text",
                "text": f"Connection '{name}' opened to {home}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error opening connection: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error opening connection: {str(e)}"
            }]
        }

@mcp.tool()
async def close_connection(
    ctx: Context,
    name: str = PydanticField(description=CONN_DESCRIPTION)
) -> Dict:
    """
    Close a named WiredTiger connection and all its cursors.

    When to use this tool:
        - When you are done debugging a database and want to release the lock
        - Before opening the same database with different configuration
    """
    try:
        info = _get_connection(name)
        await ctx.info(f"Closing connection '{name}'")

        # Close all cursors belonging to this connection
        to_remove = [cname for cname, cinfo in _cursors.items() if cinfo.connection_name == name]
        for cname in to_remove:
            try:
                _cursors[cname].cursor.close()
            except Exception:
                pass
            del _cursors[cname]

        try:
            info.session.close()
        finally:
            info.conn.close()
        del _connections[name]

        msg = f"Connection '{name}' closed."
        if to_remove:
            msg += f" Also closed {len(to_remove)} cursor(s): {', '.join(to_remove)}"
        return {
            "content": [{
                "type": "text",
                "text": msg
            }]
        }

    except Exception as e:
        await ctx.error(f"Error closing connection: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error closing connection: {str(e)}"
            }]
        }

@mcp.tool()
async def list_connections(ctx: Context) -> Dict:
    """
    List all open WiredTiger connections and their cursors.

    When to use this tool:
        - To see what connections are currently open
        - To find a connection name for use in other tools
        - To check what cursors are open on each connection
    """
    if not _connections:
        return {
            "content": [{
                "type": "text",
                "text": "No open connections."
            }]
        }

    result = []
    for name, info in _connections.items():
        cursors = [
            {"name": cname, "uri": cinfo.uri}
            for cname, cinfo in _cursors.items()
            if cinfo.connection_name == name
        ]
        result.append({
            "name": name,
            "home": info.home,
            "config": info.config,
            "cursors": cursors
        })

    return {
        "content": [{
            "type": "text",
            "text": json.dumps(result, indent=2)
        }]
    }

# ---------------------------------------------------------------------------
# Cursor tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def open_cursor(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION),
    cursor_config: Optional[str] = PydanticField(default=None, description="Cursor configuration string, e.g. 'checkpoint=WiredTigerCheckpoint' or 'dump=json'.")
) -> Dict:
    """
    Open a cursor on a URI within an existing connection.

    Only row-store tables are supported. Column-store tables (key_format='r')
    are rejected.

    Returns a cursor name that can be used with cursor_next, cursor_prev,
    cursor_search, cursor_search_near, and close_cursor.

    When to use this tool:
        - To start reading data from a table or file
        - To open a cursor at a specific checkpoint
        - To iterate through records in a table
    """
    global _cursor_counter

    try:
        info = _get_connection(connection)

        # Look up formats from metadata and reject column-store
        metadata_value = metadata_search(info.session, uri)
        key_format = ""
        value_format = ""
        if metadata_value is not None:
            key_format = _parse_config_field(metadata_value, "key_format") or ""
            value_format = _parse_config_field(metadata_value, "value_format") or ""

        if 'r' in key_format:
            raise ValueError(
                f"Column-store tables (key_format='r') are not supported by cursor operations. "
                f"URI '{uri}' has key_format='{key_format}'."
            )

        await ctx.info(f"Opening cursor on {uri} (connection '{connection}')")

        cursor = info.session.open_cursor(uri, None, cursor_config)
        _cursor_counter += 1
        cursor_name = f"cursor_{_cursor_counter}"
        _cursors[cursor_name] = CursorInfo(
            cursor=cursor,
            connection_name=connection,
            uri=uri,
            key_format=key_format,
            value_format=value_format,
        )

        return {
            "content": [{
                "type": "text",
                "text": f"Cursor '{cursor_name}' opened on {uri}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error opening cursor: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error opening cursor: {str(e)}"
            }]
        }

@mcp.tool()
async def close_cursor(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION)
) -> Dict:
    """
    Close a named cursor.

    When to use this tool:
        - When you are done reading from a cursor
        - Before opening a new cursor on the same URI with different configuration
    """
    try:
        info = _get_cursor(cursor)
        info.cursor.close()
        del _cursors[cursor]

        return {
            "content": [{
                "type": "text",
                "text": f"Cursor '{cursor}' closed."
            }]
        }

    except Exception as e:
        await ctx.error(f"Error closing cursor: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error closing cursor: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_next(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION),
    count: int = PydanticField(default=10, description="Number of records to advance. Defaults to 10.")
) -> Dict:
    """
    Advance a cursor forward and return the next N key-value pairs.

    When to use this tool:
        - To scan through records in a table
        - To paginate through results incrementally
        - To inspect what data exists after the current cursor position
    """
    try:
        info = _get_cursor(cursor)
        records = []
        for _ in range(count):
            ret = info.cursor.next()
            if ret != 0:
                break
            key = info.cursor.get_key()
            value = info.cursor.get_value()
            records.append({"key": key, "value": value})

        result = {
            "cursor": cursor,
            "records_returned": len(records),
            "records_requested": count,
            "end_of_data": len(records) < count,
            "records": records
        }

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(result, indent=2, default=str)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error advancing cursor: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error advancing cursor: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_prev(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION),
    count: int = PydanticField(default=10, description="Number of records to step backward. Defaults to 10.")
) -> Dict:
    """
    Step a cursor backward and return the previous N key-value pairs.

    When to use this tool:
        - To scan through records in reverse order
        - To look at records before the current cursor position
    """
    try:
        info = _get_cursor(cursor)
        records = []
        for _ in range(count):
            ret = info.cursor.prev()
            if ret != 0:
                break
            key = info.cursor.get_key()
            value = info.cursor.get_value()
            records.append({"key": key, "value": value})

        result = {
            "cursor": cursor,
            "records_returned": len(records),
            "records_requested": count,
            "end_of_data": len(records) < count,
            "records": records
        }

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(result, indent=2, default=str)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error stepping cursor backward: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error stepping cursor backward: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_search(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION),
    key: str = PydanticField(description="The key to search for. Will be converted to the appropriate type based on the table's key_format.")
) -> Dict:
    """
    Search for an exact key in a cursor.

    When to use this tool:
        - To look up a specific key in a table
        - To check if a key exists
        - To position the cursor at a specific key before calling cursor_next or cursor_prev
    """
    try:
        info = _get_cursor(cursor)

        converted_key = _convert_key(info.key_format, key)
        info.cursor.set_key(converted_key)
        ret = info.cursor.search()

        if ret == 0:
            value = info.cursor.get_value()
            return {
                "content": [{
                    "type": "text",
                    "text": json.dumps({"found": True, "key": key, "value": value}, indent=2, default=str)
                }]
            }
        else:
            return {
                "content": [{
                    "type": "text",
                    "text": json.dumps({"found": False, "key": key}, indent=2)
                }]
            }

    except Exception as e:
        await ctx.error(f"Error searching cursor: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error searching cursor: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_search_near(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION),
    key: str = PydanticField(description="The key to search near. Will be converted to the appropriate type based on the table's key_format.")
) -> Dict:
    """
    Search for the nearest key in a cursor.

    Returns the found key/value and a comparison indicator:
      -1 = the found key is smaller than the search key
       0 = exact match
       1 = the found key is larger than the search key

    The cursor is positioned at the found key, so you can call cursor_next
    or cursor_prev to explore the neighborhood.

    When to use this tool:
        - To find the closest key when an exact match may not exist
        - To position the cursor near a key range boundary
        - To explore what keys exist near a specific value
    """
    try:
        info = _get_cursor(cursor)

        converted_key = _convert_key(info.key_format, key)
        info.cursor.set_key(converted_key)
        ret = info.cursor.search_near()

        found_key = info.cursor.get_key()
        found_value = info.cursor.get_value()

        comparison_labels = {-1: "smaller", 0: "exact", 1: "larger"}
        result = {
            "found": True,
            "searched_key": key,
            "found_key": found_key,
            "found_value": found_value,
            "comparison": ret,
            "comparison_meaning": comparison_labels.get(ret, f"unknown ({ret})")
        }

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(result, indent=2, default=str)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error in search_near: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error in search_near: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_reset(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION)
) -> Dict:
    """
    Reset a cursor to its initial position (before the first record).

    When to use this tool:
        - To re-scan a table from the beginning
        - To release any position held by the cursor
    """
    try:
        info = _get_cursor(cursor)
        info.cursor.reset()

        return {
            "content": [{
                "type": "text",
                "text": f"Cursor '{cursor}' reset."
            }]
        }

    except Exception as e:
        await ctx.error(f"Error resetting cursor: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error resetting cursor: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_largest_key(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION)
) -> Dict:
    """
    Find the largest key in a table.

    Positions the cursor at the largest key without scanning the entire table.
    Returns WT_NOTFOUND if the table is empty.

    When to use this tool:
        - To quickly find the maximum key in a table
        - To understand data bounds without a full scan
    """
    try:
        info = _get_cursor(cursor)
        ret = info.cursor.largest_key()

        if ret == wiredtiger.WT_NOTFOUND:
            return {
                "content": [{
                    "type": "text",
                    "text": json.dumps({"found": False, "message": "Table is empty."}, indent=2)
                }]
            }

        key = info.cursor.get_key()
        return {
            "content": [{
                "type": "text",
                "text": json.dumps({"found": True, "largest_key": key}, indent=2, default=str)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error finding largest key: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error finding largest key: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_insert(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION),
    key: str = PydanticField(description=(
        "The key to insert. For integer types (i/l/q/etc.) provide a number as a string, "
        "e.g. '42'. For string keys (S) provide the string directly. "
        "For raw bytes (u) provide the UTF-8 text to encode."
    )),
    value: str = PydanticField(description=(
        "The value to insert. For integer types provide a number as a string, e.g. '99'. "
        "For string values (S) provide the string directly. "
        "For raw bytes (u) provide the UTF-8 text to encode."
    )),
) -> Dict:
    """
    Insert or overwrite a key-value pair using an open cursor.

    Only row-store tables are supported. Column-store tables (key_format='r')
    are not supported by cursor operations.

    The key and value strings are automatically converted to the correct Python
    types based on the table's key_format and value_format metadata.

    Supported key/value formats:
      S  - NUL-terminated string
      i  - 32-bit signed integer
      l  - 64-bit signed integer
      q  - 64-bit signed integer
      u  - raw byte array
      b/B/h/H/I/L/Q/t - various integer widths

    When to use this tool:
        - To write test data into a table
        - To populate a newly created table with records
        - To overwrite an existing record
    """
    try:
        info = _get_cursor(cursor)

        converted_key = _convert_key(info.key_format, key)
        converted_value = _convert_value(info.value_format, value)

        info.cursor.set_key(converted_key)
        info.cursor.set_value(converted_value)
        info.cursor.insert()

        return {
            "content": [{
                "type": "text",
                "text": json.dumps({
                    "inserted": True,
                    "cursor": cursor,
                    "key": key,
                    "value": value,
                    "converted_key": str(converted_key),
                    "converted_value": str(converted_value),
                }, indent=2)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error inserting record: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error inserting record: {str(e)}"
            }]
        }

@mcp.tool()
async def cursor_remove(
    ctx: Context,
    cursor: str = PydanticField(description=CURSOR_DESCRIPTION),
    key: str = PydanticField(description="The key to remove. Converted to the appropriate type based on the table's key_format."),
) -> Dict:
    """
    Remove a key-value pair from a table using an open cursor.

    When to use this tool:
        - To delete a specific record from a table
        - To clean up test data
    """
    try:
        info = _get_cursor(cursor)

        converted_key = _convert_key(info.key_format, key)
        info.cursor.set_key(converted_key)
        info.cursor.remove()

        return {
            "content": [{
                "type": "text",
                "text": json.dumps({"removed": True, "key": key}, indent=2)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error removing record: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error removing record: {str(e)}"
            }]
        }

# ---------------------------------------------------------------------------
# Transaction tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def begin_transaction(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    config: Optional[str] = PydanticField(default=None, description="Transaction configuration, e.g. 'read_timestamp=0x10' or 'isolation=snapshot'.")
) -> Dict:
    """
    Begin a transaction on a connection's session.

    When to use this tool:
        - To read data at a specific timestamp using read_timestamp
        - To set transaction isolation level
        - To start a transactional context for multiple cursor operations
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Beginning transaction on connection '{connection}'")
        info.session.begin_transaction(config or "")

        msg = f"Transaction started on connection '{connection}'"
        if config:
            msg += f" (config: {config})"
        return {
            "content": [{
                "type": "text",
                "text": msg
            }]
        }

    except Exception as e:
        await ctx.error(f"Error beginning transaction: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error beginning transaction: {str(e)}"
            }]
        }

@mcp.tool()
async def commit_transaction(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    config: Optional[str] = PydanticField(default=None, description="Commit configuration, e.g. 'commit_timestamp=0x10'.")
) -> Dict:
    """
    Commit the active transaction on a connection's session.

    When to use this tool:
        - To finalize a transaction after reading data
        - After completing a set of transactional operations
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Committing transaction on connection '{connection}'")
        info.session.commit_transaction(config or "")

        return {
            "content": [{
                "type": "text",
                "text": f"Transaction committed on connection '{connection}'."
            }]
        }

    except Exception as e:
        await ctx.error(f"Error committing transaction: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error committing transaction: {str(e)}"
            }]
        }

@mcp.tool()
async def rollback_transaction(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION)
) -> Dict:
    """
    Roll back the active transaction on a connection's session.

    When to use this tool:
        - To abort a transaction without committing
        - To release read locks after inspecting data at a timestamp
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Rolling back transaction on connection '{connection}'")
        info.session.rollback_transaction("")

        return {
            "content": [{
                "type": "text",
                "text": f"Transaction rolled back on connection '{connection}'."
            }]
        }

    except Exception as e:
        await ctx.error(f"Error rolling back transaction: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error rolling back transaction: {str(e)}"
            }]
        }

# ---------------------------------------------------------------------------
# Schema tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def list_tables(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION)
) -> Dict:
    """
    List all tables, files, and other objects in the WiredTiger metadata.

    Returns every key in the metadata cursor, including tables, files, column
    groups, and indexes.

    When to use this tool:
        - To discover what tables and files exist in the database
        - To find the correct URI for use with other tools
        - To get an overview of the database schema
    """
    cursor = None

    try:
        info = _get_connection(connection)
        await ctx.info(f"Listing tables on connection '{connection}'")

        cursor = info.session.open_cursor("metadata:", None, None)
        entries = []
        while cursor.next() == 0:
            entries.append(cursor.get_key())

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(entries, indent=2)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error listing tables: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error listing tables: {str(e)}"
            }]
        }

    finally:
        if cursor:
            cursor.close()

@mcp.tool()
async def get_metadata(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION)
) -> Dict:
    """
    Get the raw metadata string for a specific URI.

    Returns the full metadata configuration string as stored in the WiredTiger
    metadata table, including key_format, value_format, columns, collator,
    block allocation size, checkpoints, and all other properties.

    When to use this tool:
        - To inspect the full configuration of a table or file
        - To check key_format and value_format
        - To examine checkpoint metadata
        - To debug schema issues
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Getting metadata for {uri}")

        value = metadata_search(info.session, uri)
        if value is not None:
            return {
                "content": [{
                    "type": "text",
                    "text": json.dumps({"uri": uri, "metadata": value}, indent=2)
                }]
            }

        return {
            "content": [{
                "type": "text",
                "text": f"URI not found in metadata: {uri}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error getting metadata: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error getting metadata: {str(e)}"
            }]
        }

@mcp.tool()
async def get_schema(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION)
) -> Dict:
    """
    Get a parsed schema summary for a table or file.

    Extracts key schema properties from the metadata: key_format, value_format,
    columns, colgroups, source, and other structural fields. Easier to read than
    the raw metadata string.

    When to use this tool:
        - To quickly understand a table's schema (key/value formats, columns)
        - To determine the correct key type for cursor operations
        - To check the source file backing a table
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Getting schema for {uri}")

        value = metadata_search(info.session, uri)
        if value is None:
            return {
                "content": [{
                    "type": "text",
                    "text": f"URI not found in metadata: {uri}"
                }]
            }

        schema_keys = [
            "key_format", "value_format", "columns", "colgroups",
            "source", "type", "app_metadata", "collator",
            "block_allocation", "allocation_size", "leaf_page_max",
            "internal_page_max", "memory_page_max",
        ]

        schema = {"uri": uri}
        for key in schema_keys:
            field_value = _parse_config_field(value, key)
            if field_value is not None:
                schema[key] = field_value

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(schema, indent=2)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error getting schema: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error getting schema: {str(e)}"
            }]
        }

@mcp.tool()
async def create_table(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description="The URI for the new table, e.g. 'table:mytable'."),
    config: str = PydanticField(description="Table configuration string, e.g. 'key_format=S,value_format=S' or 'key_format=i,value_format=S,columns=(id,name)'.")
) -> Dict:
    """
    Create a new table or file in the WiredTiger database.

    When to use this tool:
        - To create a test table for experimentation
        - To set up a table with a specific schema
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Creating {uri} on connection '{connection}'")
        info.session.create(uri, config)

        return {
            "content": [{
                "type": "text",
                "text": f"Created {uri} with config: {config}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error creating table: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error creating table: {str(e)}"
            }]
        }

@mcp.tool()
async def drop_table(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description="The URI of the table or file to drop, e.g. 'table:mytable'."),
    force: bool = PydanticField(default=False, description="If true, return success even if the object does not exist.")
) -> Dict:
    """
    Drop a table or file from the WiredTiger database.

    This permanently removes the object and its data. Any open cursors on the
    dropped URI should be closed first.

    When to use this tool:
        - To remove a table that is no longer needed
        - To clean up test tables
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Dropping {uri} on connection '{connection}'")

        drop_config = "force=true" if force else None
        info.session.drop(uri, drop_config)

        return {
            "content": [{
                "type": "text",
                "text": f"Dropped {uri}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error dropping table: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error dropping table: {str(e)}"
            }]
        }

@mcp.tool()
async def rename_table(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    old_uri: str = PydanticField(description="The current URI of the table, e.g. 'table:oldname'."),
    new_uri: str = PydanticField(description="The new URI for the table, e.g. 'table:newname'.")
) -> Dict:
    """
    Rename a table in the WiredTiger database.

    When to use this tool:
        - To rename a table
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Renaming {old_uri} to {new_uri}")
        info.session.rename(old_uri, new_uri, None)

        return {
            "content": [{
                "type": "text",
                "text": f"Renamed {old_uri} to {new_uri}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error renaming table: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error renaming table: {str(e)}"
            }]
        }

@mcp.tool()
async def alter_table(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION),
    config: str = PydanticField(description=(
        "Alter configuration string. Options include: "
        "'access_pattern_hint=random|sequential|none', "
        "'app_metadata=<string>', "
        "'cache_resident=true|false', "
        "'log=(enabled=true|false)'."
    ))
) -> Dict:
    """
    Alter the configuration of an existing table or file.

    When to use this tool:
        - To change access pattern hints for performance tuning
        - To set application metadata on a table
        - To enable or disable transaction logging for a table
        - To change cache residency settings
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Altering {uri} on connection '{connection}'")
        info.session.alter(uri, config)

        return {
            "content": [{
                "type": "text",
                "text": f"Altered {uri} with config: {config}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error altering table: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error altering table: {str(e)}"
            }]
        }

@mcp.tool()
async def truncate(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION),
    start_key: Optional[str] = PydanticField(default=None, description="Start of the truncation range (inclusive). If omitted, truncation starts from the beginning of the table."),
    stop_key: Optional[str] = PydanticField(default=None, description="End of the truncation range (inclusive). If omitted, truncation goes to the end of the table.")
) -> Dict:
    """
    Remove a range of records from a table, or all records if no range is given.

    If neither start_key nor stop_key is provided, all records are removed.
    If only start_key is provided, all records from that key to the end are removed.
    If only stop_key is provided, all records from the beginning to that key are removed.

    When to use this tool:
        - To remove all records from a table
        - To remove a range of records
        - To clean up test data
    """
    start_cursor = None
    stop_cursor = None

    try:
        info = _get_connection(connection)
        await ctx.info(f"Truncating {uri} on connection '{connection}'")

        metadata_value = metadata_search(info.session, uri)
        key_format = ""
        if metadata_value is not None:
            key_format = _parse_config_field(metadata_value, "key_format") or ""

        if start_key is not None:
            start_cursor = info.session.open_cursor(uri, None, None)
            start_cursor.set_key(_convert_key(key_format, start_key))

        if stop_key is not None:
            stop_cursor = info.session.open_cursor(uri, None, None)
            stop_cursor.set_key(_convert_key(key_format, stop_key))

        # When using cursors, pass None for the URI
        truncate_uri = uri if start_cursor is None and stop_cursor is None else None
        info.session.truncate(truncate_uri, start_cursor, stop_cursor, None)

        range_desc = "all records"
        if start_key and stop_key:
            range_desc = f"records from {start_key} to {stop_key}"
        elif start_key:
            range_desc = f"records from {start_key} to end"
        elif stop_key:
            range_desc = f"records from beginning to {stop_key}"

        return {
            "content": [{
                "type": "text",
                "text": f"Truncated {uri}: removed {range_desc}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error truncating table: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error truncating table: {str(e)}"
            }]
        }

    finally:
        if start_cursor:
            start_cursor.close()
        if stop_cursor:
            stop_cursor.close()

# ---------------------------------------------------------------------------
# Diagnostic tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def query_timestamps(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION)
) -> Dict:
    """
    Query all global transaction timestamps for a WiredTiger connection.

    Returns: all_durable, last_checkpoint, oldest_timestamp, oldest_reader,
    pinned, recovery, and stable_timestamp as hex-encoded values.

    When to use this tool:
        - To understand the temporal state of the database
        - To debug transaction visibility or timestamp ordering issues
        - To check if timestamps have been set correctly
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Querying timestamps on connection '{connection}'")

        timestamp_types = [
            "all_durable",
            "last_checkpoint",
            "oldest_timestamp",
            "oldest_reader",
            "pinned",
            "recovery",
            "stable_timestamp",
        ]

        timestamps = {}
        for ts_type in timestamp_types:
            try:
                value = info.conn.query_timestamp(f"get={ts_type}")
                timestamps[ts_type] = value
            except wiredtiger.WiredTigerError as e:
                timestamps[ts_type] = f"unavailable ({str(e)})"

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(timestamps, indent=2)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error querying timestamps: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error querying timestamps: {str(e)}"
            }]
        }

@mcp.tool()
async def verify_table(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION),
    dump_address: bool = PydanticField(default=False, description="Display page addresses, time windows, and page types"),
    dump_all_data: bool = PydanticField(default=False, description="Display all application data found during verification"),
    dump_key_data: bool = PydanticField(default=False, description="Display keys found during verification"),
    dump_blocks: bool = PydanticField(default=False, description="Display on-disk block contents"),
    dump_pages: bool = PydanticField(default=False, description="Display in-memory page contents"),
    dump_layout: bool = PydanticField(default=False, description="Display file layout information"),
    dump_tree_shape: bool = PydanticField(default=False, description="Display the B-tree shape"),
    read_corrupt: bool = PydanticField(default=False, description="Continue verification after checksum errors, skipping corrupt blocks"),
    strict: bool = PydanticField(default=False, description="Treat verification warnings as errors"),
    stable_timestamp: bool = PydanticField(default=False, description="Verify no data exists after the stable timestamp"),
) -> Dict:
    """
    Run verify on a WiredTiger table or file with configurable dump options.

    Exposes the full range of session.verify() options for detailed inspection
    of on-disk and in-memory data structures.

    Note: verify dump output goes through the WiredTiger message handler. If
    the connection was not opened with an appropriate verbose configuration,
    some output may not be captured.

    When to use this tool:
        - To check the integrity of a specific table or file
        - To inspect on-disk blocks, pages, or addresses for debugging
        - To investigate data corruption
        - To verify no data exists beyond the stable timestamp
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Running verify on {uri} (connection '{connection}')")

        verify_opts = []
        if dump_address:
            verify_opts.append("dump_address=true")
        if dump_all_data:
            verify_opts.append("dump_all_data=true")
        if dump_key_data:
            verify_opts.append("dump_key_data=true")
        if dump_blocks:
            verify_opts.append("dump_blocks=true")
        if dump_pages:
            verify_opts.append("dump_pages=true")
        if dump_layout:
            verify_opts.append("dump_layout=true")
        if dump_tree_shape:
            verify_opts.append("dump_tree_shape=true")
        if read_corrupt:
            verify_opts.append("read_corrupt=true")
        if strict:
            verify_opts.append("strict=true")
        if stable_timestamp:
            verify_opts.append("stable_timestamp=true")

        verify_config = ",".join(verify_opts) if verify_opts else None

        # Capture fd-level output from WiredTiger's C message handler.
        old_stdout_fd = os.dup(1)
        old_stderr_fd = os.dup(2)
        try:
            stdout_r, stdout_w = os.pipe()
            stderr_r, stderr_w = os.pipe()
            os.dup2(stdout_w, 1)
            os.dup2(stderr_w, 2)
            os.close(stdout_w)
            os.close(stderr_w)

            info.session.verify(uri, verify_config)

            # Restore original fds so we can read the pipes safely.
            os.dup2(old_stdout_fd, 1)
            os.dup2(old_stderr_fd, 2)

            stdout_output = b""
            while True:
                chunk = os.read(stdout_r, 4096)
                if not chunk:
                    break
                stdout_output += chunk
            stderr_output = b""
            while True:
                chunk = os.read(stderr_r, 4096)
                if not chunk:
                    break
                stderr_output += chunk

            os.close(stdout_r)
            os.close(stderr_r)
        except Exception:
            os.dup2(old_stdout_fd, 1)
            os.dup2(old_stderr_fd, 2)
            raise
        finally:
            os.close(old_stdout_fd)
            os.close(old_stderr_fd)

        stdout_text = stdout_output.decode("utf-8", errors="replace")
        stderr_text = stderr_output.decode("utf-8", errors="replace")

        result_text = f"Verify completed for {uri}"
        if verify_config:
            result_text += f" (options: {verify_config})"
        if stdout_text:
            result_text += f"\n\nOutput:\n{stdout_text}"
        if stderr_text:
            result_text += f"\n\nErrors:\n{stderr_text}"
        if not stdout_text and not stderr_text:
            result_text += "\n\nVerification passed with no output."

        return {
            "content": [{
                "type": "text",
                "text": result_text
            }]
        }

    except Exception as e:
        await ctx.error(f"Error verifying {uri}: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error verifying {uri}: {str(e)}"
            }]
        }

@mcp.tool()
async def dump_block(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION),
    offsets: List[str] = PydanticField(description="List of on-disk block offsets to dump, e.g. ['0', '4096', '8192']"),
) -> Dict:
    """
    Dump the contents of specific on-disk blocks from a WiredTiger file.

    Uses session.verify() with the dump_offsets option to display block contents
    at the specified byte offsets.

    When to use this tool:
        - To investigate corruption at known block offsets
        - To inspect the raw contents of specific on-disk blocks
        - To examine blocks identified from a B-tree layout analysis
    """
    try:
        info = _get_connection(connection)
        offset_list = ",".join(offsets)
        verify_config = f"dump_offsets=[{offset_list}]"
        await ctx.info(f"Dumping blocks at offsets [{offset_list}] from {uri}")

        # Capture fd-level output from WiredTiger's C message handler.
        old_stdout_fd = os.dup(1)
        old_stderr_fd = os.dup(2)
        try:
            stdout_r, stdout_w = os.pipe()
            stderr_r, stderr_w = os.pipe()
            os.dup2(stdout_w, 1)
            os.dup2(stderr_w, 2)
            os.close(stdout_w)
            os.close(stderr_w)

            info.session.verify(uri, verify_config)

            os.dup2(old_stdout_fd, 1)
            os.dup2(old_stderr_fd, 2)

            stdout_output = b""
            while True:
                chunk = os.read(stdout_r, 4096)
                if not chunk:
                    break
                stdout_output += chunk
            stderr_output = b""
            while True:
                chunk = os.read(stderr_r, 4096)
                if not chunk:
                    break
                stderr_output += chunk

            os.close(stdout_r)
            os.close(stderr_r)
        except Exception:
            os.dup2(old_stdout_fd, 1)
            os.dup2(old_stderr_fd, 2)
            raise
        finally:
            os.close(old_stdout_fd)
            os.close(old_stderr_fd)

        stdout_text = stdout_output.decode("utf-8", errors="replace")
        stderr_text = stderr_output.decode("utf-8", errors="replace")

        result_text = f"Block dump for {uri} at offsets [{offset_list}]"
        if stdout_text:
            result_text += f"\n\nOutput:\n{stdout_text}"
        if stderr_text:
            result_text += f"\n\nErrors:\n{stderr_text}"
        if not stdout_text and not stderr_text:
            result_text += "\n\nNo output produced."

        return {
            "content": [{
                "type": "text",
                "text": result_text
            }]
        }

    except Exception as e:
        await ctx.error(f"Error dumping blocks from {uri}: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error dumping blocks from {uri}: {str(e)}"
            }]
        }

@mcp.tool()
async def get_statistics_by_category(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    category: str = PydanticField(description="A keyword to filter statistics by, e.g. 'cache', 'eviction', 'checkpoint', 'transaction', 'cursor', 'btree', 'log', 'lock', 'compact'."),
    uri: Optional[str] = PydanticField(default=None, description="Optional URI to get file/table-level statistics instead of connection-level."),
) -> Dict:
    """
    Get WiredTiger statistics filtered by a keyword category.

    Retrieves connection-level or file/table-level statistics and filters them
    to only include entries whose description matches the given keyword. This
    avoids returning the full statistics set (hundreds of entries) and keeps
    results focused and context-window friendly.

    Note: the connection must have been opened with 'statistics=(all)' in its
    config for this tool to work.

    When to use this tool:
        - To analyze performance metrics for a specific subsystem
        - To get cache, eviction, checkpoint, or transaction statistics
        - To focus on file-level statistics for a specific table
    """
    stat_cursor = None

    try:
        info = _get_connection(connection)
        stat_target = uri if uri else "connection"
        await ctx.info(f"Getting '{category}' statistics for {stat_target}")

        cursor_uri = f"statistics:{uri}" if uri else "statistics:"
        stat_cursor = info.session.open_cursor(cursor_uri, None, None)

        category_lower = category.lower()
        stats = []
        total_count = 0
        while stat_cursor.next() == 0:
            total_count += 1
            value = stat_cursor.get_value()
            # WiredTiger stat cursor value is a 3-element sequence: (description, printable, numeric)
            try:
                desc = value[0]
                value_str = str(value[1])
            except (IndexError, TypeError):
                desc = str(stat_cursor.get_key())
                value_str = str(value)
            if category_lower in str(desc).lower():
                stats.append({
                    "description": desc,
                    "value": value,
                    "printableValue": value_str
                })

        result = {
            "category": category,
            "target": stat_target,
            "matched": len(stats),
            "total_statistics": total_count,
            "statistics": stats
        }

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(result, indent=2, default=str)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error retrieving statistics: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error retrieving statistics: {str(e)}"
            }]
        }

    finally:
        if stat_cursor:
            stat_cursor.close()

@mcp.tool()
async def checkpoint(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    config: Optional[str] = PydanticField(default=None, description=(
        "Checkpoint configuration. Options include: "
        "'name=<string>' to name the checkpoint, "
        "'force=true' to force even if unmodified, "
        "'drop=(from=all)' to drop named checkpoints."
    ))
) -> Dict:
    """
    Force a checkpoint on the database.

    A checkpoint writes all modified in-memory data to disk, creating a
    consistent on-disk snapshot.

    When to use this tool:
        - To ensure on-disk state is current before running verify or dump_block
        - To create a named checkpoint for later inspection
        - To drop old named checkpoints
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Running checkpoint on connection '{connection}'")
        info.session.checkpoint(config or "")

        msg = f"Checkpoint completed on connection '{connection}'"
        if config:
            msg += f" (config: {config})"
        return {
            "content": [{
                "type": "text",
                "text": msg
            }]
        }

    except Exception as e:
        await ctx.error(f"Error running checkpoint: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error running checkpoint: {str(e)}"
            }]
        }

@mcp.tool()
async def reconfigure_connection(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    config: str = PydanticField(description=(
        "Reconfiguration string."
    ))
) -> Dict:
    """
    Reconfigure a live WiredTiger connection at runtime.

    Allows changing connection settings without closing and reopening,
    such as enabling statistics, adjusting cache size, or toggling verbose
    logging.

    When to use this tool:
        - To enable statistics=(all) after opening without it
        - To change cache size for testing
        - To toggle verbose logging for specific subsystems
        - To adjust eviction settings
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Reconfiguring connection '{connection}'")
        info.conn.reconfigure(config)

        return {
            "content": [{
                "type": "text",
                "text": f"Connection '{connection}' reconfigured with: {config}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error reconfiguring connection: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error reconfiguring connection: {str(e)}"
            }]
        }

@mcp.tool()
async def set_timestamp(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    config: str = PydanticField(description=(
        "Timestamp configuration."
    ))
) -> Dict:
    """
    Set global transaction timestamps on a connection.

    These timestamps control transaction visibility and checkpoint boundaries.
    Use query_timestamps to read the current values.

    When to use this tool:
        - To set oldest/stable timestamps before running rollback_to_stable
        - To advance timestamps for testing temporal operations
        - To configure checkpoint boundaries
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Setting timestamp on connection '{connection}'")
        info.conn.set_timestamp(config)

        return {
            "content": [{
                "type": "text",
                "text": f"Timestamps set on connection '{connection}': {config}"
            }]
        }

    except Exception as e:
        await ctx.error(f"Error setting timestamp: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error setting timestamp: {str(e)}"
            }]
        }

@mcp.tool()
async def rollback_to_stable(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    config: Optional[str] = PydanticField(default=None, description=(
        "RTS configuration."
    ))
) -> Dict:
    """
    Roll back the database to the stable timestamp.

    Discards all data modifications more recent than the stable timestamp.
    All running transactions must be resolved and all open cursors must be
    closed or reset before calling this.

    When to use this tool:
        - To test rollback-to-stable behavior
        - To restore the database to a known-good state
        - To discard uncommitted or unstable changes
    """
    try:
        info = _get_connection(connection)
        await ctx.info(f"Running rollback_to_stable on connection '{connection}'")
        info.conn.rollback_to_stable(config or "")

        msg = f"Rollback to stable completed on connection '{connection}'"
        if config:
            msg += f" (config: {config})"
        return {
            "content": [{
                "type": "text",
                "text": msg
            }]
        }

    except Exception as e:
        await ctx.error(f"Error in rollback_to_stable: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error in rollback_to_stable: {str(e)}"
            }]
        }

@mcp.tool()
async def read_log(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    limit: int = PydanticField(default=50, description="Maximum number of log records to return. Defaults to 50.")
) -> Dict:
    """
    Read write-ahead log (WAL) entries from the database.

    Opens a log cursor and iterates over WAL records, returning the LSN,
    transaction ID, record type, operation type, file ID, and key/value data
    for each entry.

    The connection must have been opened with logging enabled for this to work.

    When to use this tool:
        - To understand what operations were journaled
        - To debug crash recovery issues
        - To correlate log entries with on-disk state
        - To inspect transaction history
    """
    cursor = None

    try:
        info = _get_connection(connection)
        await ctx.info(f"Reading log on connection '{connection}'")

        cursor = info.session.open_cursor("log:", None, None)
        records = []
        count = 0
        while cursor.next() == 0 and count < limit:
            count += 1
            keys = cursor.get_key()
            values = cursor.get_value()

            record = {
                "lsn_file": keys[0],
                "lsn_offset": keys[1],
                "opcount": keys[2],
                "txnid": values[0],
                "rectype": values[1],
                "optype": values[2],
                "fileid": values[3],
            }
            # Include key/value bytes if non-empty
            if values[4]:
                record["logrec_key"] = values[4].hex()
            if values[5]:
                record["logrec_value"] = values[5].hex()

            records.append(record)

        result = {
            "connection": connection,
            "records_returned": len(records),
            "limit": limit,
            "records": records
        }

        return {
            "content": [{
                "type": "text",
                "text": json.dumps(result, indent=2, default=str)
            }]
        }

    except Exception as e:
        await ctx.error(f"Error reading log: {str(e)}")
        return {
            "content": [{
                "type": "text",
                "text": f"Error reading log: {str(e)}"
            }]
        }

    finally:
        if cursor:
            cursor.close()

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="WiredTiger MCP Server")
    parser.add_argument(
        "--log-level",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        default="DEBUG",
        help="Set the logging level (default: DEBUG)"
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        default=True,
        help="Enable debug mode"
    )
    args = parser.parse_args()

    configure_logging(level=args.log_level)
    logger.info(f"Starting WiredTiger MCP Server with log level: {args.log_level}")

    if args.debug:
        logger.info("Debug mode enabled")

    mcp.run(transport="stdio")

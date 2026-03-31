# WiredTiger MCP Server

An MCP server with persistent connection management for WiredTiger databases. Connections, sessions, cursors, and transactions are managed explicitly through tools, allowing stateful debugging workflows across multiple tool calls.

## Overview

The server maintains persistent state:
- **Connections** are opened by name and stay open until explicitly closed
- **Cursors** are opened on a connection and can be stepped through incrementally
- **Transactions** can be started on a connection to read at specific timestamps

This enables debugging workflows such as:
- Scanning a table page by page
- Reading data at a specific timestamp
- Comparing cursor positions across multiple tables
- Exploring key neighborhoods with search_near + next/prev
- Rolling back to a stable timestamp and inspecting results
- Reading WAL log entries for crash recovery analysis

### What is MCP?

The Model Context Protocol (MCP) is a standardised way for AI assistants to interact with external tools, resources, and environments. For more information, visit [modelcontextprotocol.io](https://modelcontextprotocol.io).

## Installation

### Prerequisites

- Python 3.13
- uv ([astral.sh/uv](https://astral.sh/uv))
- WiredTiger Python API (built from source)
- MCP Python SDK

### Setup

1. Install `uv` if you haven't already:

   ```bash
   # On macOS and Linux.
   curl -LsSf https://astral.sh/uv/install.sh | sh
   ```

   Or via pip: `pip install uv`

2. Set the `WT_BUILDDIR` environment variable. Either export it or create a `.env` file in the `wt-mcp` directory:

   ```bash
   WT_BUILDDIR=/path/to/wiredtiger/build
   ```

## Usage

### Running the Server in Claude Code

The repository includes a `.mcp.json` file at the project root that automatically configures the MCP server for Claude Code. When you open Claude Code in the WiredTiger directory, it will detect this file and offer to start the server — no manual setup required.

If you prefer to add the server manually:

```bash
claude mcp add wt-mcp -- uv --directory /path/to/wiredtiger/tools/wt-mcp run server.py
```

Ensure the `WT_BUILDDIR` environment variable is set before starting Claude Code, or configure it in a `.env` file in the `wt-mcp` directory.

See the [Claude Code MCP documentation](https://docs.anthropic.com/en/docs/claude-code/mcp) for more details.

### Running the Server in VS Code

Create a `mcp.json` file in the `.vscode` directory at the root level of your project:

```json
{
    "servers": {
        "wt-mcp": {
            "type": "stdio",
            "command": "path/to/uv",
            "args": [
                "--directory",
                "path/to/wiredtiger/tools/wt-mcp",
                "run",
                "server.py"
            ]
        }
    }
}
```

See the [MCP servers in VS Code](https://code.visualstudio.com/docs/copilot/chat/mcp-servers) documentation for more details.

### Testing with the MCP Inspector

```bash
npx @modelcontextprotocol/inspector uv --directory ~/wiredtiger/tools/wt-mcp run server.py
```

The Inspector opens a web interface (typically at <http://localhost:3000>) where you can view tools, test them with custom inputs, and review logs.

## Available Tools

### Connection Lifecycle

| Tool | Description |
|------|-------------|
| `open_connection` | Open a persistent WiredTiger connection by name |
| `close_connection` | Close a connection and all its cursors |
| `list_connections` | List all open connections and their cursors |
| `reconfigure_connection` | Reconfigure a live connection (statistics, cache size, verbose logging, eviction) |

### Cursor Operations

| Tool | Description |
|------|-------------|
| `open_cursor` | Open a cursor on a URI within a connection (row-store only) |
| `close_cursor` | Close a named cursor |
| `cursor_next` | Advance forward N records |
| `cursor_prev` | Step backward N records |
| `cursor_search` | Exact key lookup |
| `cursor_search_near` | Find nearest key (returns match direction) |
| `cursor_reset` | Reset cursor to initial position |
| `cursor_largest_key` | Find the largest key without scanning |
| `cursor_insert` | Insert or overwrite a key-value pair |
| `cursor_remove` | Remove a record by key |

### Transaction Control

| Tool | Description |
|------|-------------|
| `begin_transaction` | Start a transaction (supports `read_timestamp`, `isolation`) |
| `commit_transaction` | Commit the active transaction |
| `rollback_transaction` | Roll back the active transaction |

### Schema Operations

| Tool | Description |
|------|-------------|
| `list_tables` | List all tables, files, and objects in the metadata |
| `get_metadata` | Get the raw metadata string for a URI |
| `get_schema` | Get a parsed schema summary (key/value formats, columns, etc.) |
| `create_table` | Create a new table with a given configuration |
| `drop_table` | Drop a table or file |
| `rename_table` | Rename a table |
| `alter_table` | Alter table configuration (access pattern, app_metadata, logging) |
| `truncate` | Remove a range of records or all records from a table |

### Diagnostics

| Tool | Description |
|------|-------------|
| `query_timestamps` | Query all global timestamps (oldest, stable, pinned, etc.) |
| `set_timestamp` | Set global timestamps (oldest, stable, durable) |
| `rollback_to_stable` | Roll back the database to the stable timestamp |
| `checkpoint` | Force a checkpoint, with optional name/force/drop config |
| `verify_table` | Run verify with full dump options (addresses, blocks, pages, layout, tree shape, etc.) |
| `dump_block` | Dump specific on-disk blocks by byte offset |
| `get_statistics_by_category` | Filter statistics by keyword (cache, eviction, checkpoint, etc.) |
| `read_log` | Read write-ahead log entries (LSN, transaction ID, operation type, key/value data) |

## Example Workflows

### Basic debugging session

```
1. open_connection(name="db", home="/data/wt", config="statistics=(all)")
2. query_timestamps(connection="db")
3. list_tables(connection="db")
4. get_schema(connection="db", uri="table:mytable")
5. open_cursor(connection="db", uri="table:mytable")         -> cursor_1
6. cursor_next(cursor="cursor_1", count=5)                    -> first 5 records
7. cursor_search_near(cursor="cursor_1", key="suspect_key")   -> nearest match
8. cursor_next(cursor="cursor_1", count=3)                    -> neighborhood
9. cursor_largest_key(cursor="cursor_1")                      -> max key
10. get_statistics_by_category(connection="db", category="cache")
11. verify_table(connection="db", uri="file:mytable.wt", dump_tree_shape=true)
12. close_cursor(cursor="cursor_1")
13. close_connection(name="db")
```

### Rollback-to-stable workflow

```
1. open_connection(name="db", home="/data/wt")
2. set_timestamp(connection="db", config="stable_timestamp=0000000000000005")
3. rollback_to_stable(connection="db")
4. open_cursor(connection="db", uri="table:mytable")          -> cursor_1
5. cursor_next(cursor="cursor_1", count=10)                   -> verify state
6. close_cursor(cursor="cursor_1")
7. close_connection(name="db")
```

### WAL log inspection

```
1. open_connection(name="db", home="/data/wt")
2. read_log(connection="db", limit=20)
3. close_connection(name="db")
```

### Runtime reconfiguration

```
1. open_connection(name="db", home="/data/wt")
2. reconfigure_connection(connection="db", config="statistics=(all)")
3. get_statistics_by_category(connection="db", category="cache")
4. reconfigure_connection(connection="db", config="verbose=[checkpoint:2]")
5. checkpoint(connection="db", config="force=true")
6. close_connection(name="db")
```

## Notes

- Only row-store tables are supported by cursor operations. Column-store tables (`key_format='r'`) are rejected at `open_cursor` time.
- All connections are automatically closed when the server shuts down.
- The `get_statistics_by_category` tool requires the connection to have statistics enabled. Use `reconfigure_connection` to enable `statistics=(all)` at runtime if needed.
- Cursor key and value types are automatically converted based on the table's `key_format` and `value_format` metadata. Supported formats: `S` (string), `u` (bytes), `i`/`q`/`Q`/`b`/`B`/`h`/`H`/`I`/`l`/`L`/`t` (integers).
- Verify and dump_block output is captured at the file descriptor level to include output from WiredTiger's C message handler.
- `rollback_to_stable` requires all cursors to be closed or reset and all transactions to be resolved before calling.

## Debugging and Logging

The server provides detailed logging. Adjust the log level with `--log-level`:

```bash
uv run server.py --log-level DEBUG
```

## Extending the Server

To add new tools:

1. Define a new async function with the `@mcp.tool()` decorator
2. Accept `connection: str` as the first parameter (after `ctx`) to use a named connection
3. Use `_get_connection(connection)` to retrieve the `ConnectionInfo`
4. Document the tool with a docstring including a "When to use this tool" section
5. Return results in MCP format

Example:

```python
@mcp.tool()
async def my_new_tool(
    ctx: Context,
    connection: str = PydanticField(description=CONN_DESCRIPTION),
    uri: str = PydanticField(description=URI_DESCRIPTION),
) -> Dict:
    """
    Description of what my_new_tool does.

    When to use this tool:
        - Use case 1
        - Use case 2
    """
    info = _get_connection(connection)
    # Use info.session, info.conn, etc.
    return {
        "content": [{
            "type": "text",
            "text": "Result"
        }]
    }
```

## Additional Resources

- [MCP Python SDK Documentation](https://github.com/modelcontextprotocol/python-sdk)
- [Model Context Protocol Specification](https://modelcontextprotocol.io)
- [MCP Quickstart Guide](https://modelcontextprotocol.io/quickstart/server)

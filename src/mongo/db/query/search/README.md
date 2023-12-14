# Search Indexes

Search indexes are supported external to MongoDB servers. They are an enterprise only feature.
Mongod/s servers communicate with cluster external servers to service search index commands and
queries. There is a mongotmock binary with which tests are run that simulates the remote search
index management servers.

Currently search index queries communicate with Lucene instances wrapped into a mongot binary that
uses the same communication protocols as mongod/s servers. Search index commands similarly use
mongod/s server communication protocols to communicate with a remote serach index server, but with
an Envoy instance that handles forwarding the command requests to Atlas servers and then eventually
to the relevant Lucene/mongot instances. mongot and Envoy instances are co-located with every mongod
server instance, and Envoy instances are co-located with mongos servers as well. The precise
structure of the search index servers will likely evolve in future as improvements are made to that
system.

# Search Index Commands

There are four search index metadata commands: `createSearchIndexes`, `updateSearchIndex`,
`dropSearchIndex` and `listSearchIndexes`. These commands are present on both the mongod and mongos
and are passthrough commands to a remote search index management server. The mongod/s is aware of the
address of the remote management server via a startup setParameter `searchIndexManagementHostAndPort`.

The `mongotmock` is a separate binary used for testing search index commands and queries. The
`mongotmock` has a `manageSearchIndex` command, which the search index commands use to forward
requests to the remote search index management server. The mongotmock also has a
`setManageSearchIndexResponse` command to set the mock response for `manageSearchIndex`.

The four commands have security authorization action types corresponding with their names. These
action types are included in the same built-in roles as the regular index commands, while
`updateSearchIndex` parallels `collMod`.

_Code spelunking starting points:_

* [_The commands file_](https://github.com/10gen/mongo-enterprise-modules/blob/819e4dbc8464c1fb833543f808ed8d619bf9ca18/src/search/text_search_index_commands.cpp)
* [_The mongotmock commands file_](https://github.com/10gen/mongo-enterprise-modules/blob/819e4dbc8464c1fb833543f808ed8d619bf9ca18/src/search/mongotmock/mongotmock_commands.cpp)
* [_The main test file_](https://github.com/10gen/mongo-enterprise-modules/blob/819e4dbc8464c1fb833543f808ed8d619bf9ca18/jstests/search/text_search_index_commands.js)


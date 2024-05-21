# Views

[Views] in MongoDB are a name and an [`aggregation`][aggregation] [pipeline][pipeline] targeting some
collection or other view. They are non-materialized (or on-demand) views which means the
`aggregation` pipeline defining the view must be executed on every request. They are also read only! There is a long-standing
request ([SERVER-27698][materialized views]) to add materialized views to MongoDB, where the view
would be stored to avoid executing on each query.

Because a view is an `aggregation` pipeline, if a command is issued against a view, it's internally transformed into an 'aggregate' command. As an example of this, see [runFindOnView()] for find commands and [rewriting a count command into aggregate command].

For `aggregate` commands, we check if the command was issued against a view. If it is,
[runAggregateOnView][runAggregateOnView] is called. The view is then resolved by prepending the
view’s pipeline to the current pipeline for the query and a new call to
[runAggregate()][runAggregate()] is called with the final query.

<!-- Links -->

[aggregation]: README.md#glossary-Aggregation
[find]: README.md#glossary-Find
[pipeline]: README.md#glossary-Pipeline
[materialized views]: https://jira.mongodb.org/browse/SERVER-27698
[runFindOnView()]: https://github.com/mongodb/mongo/blob/0b7ad1d7e9aaa92666a102664abcdd84a92a2656/src/mongo/db/commands/find_cmd.cpp#L953
[find_cmd]: https://github.com/mongodb/mongo/blob/7a1d8a1ab2aa04a2fa7b2c84e5baffff43a87a27/src/mongo/db/commands/find_cmd.cpp#L720-L740
[runAggregateOnView]: https://github.com/mongodb/mongo/blob/dbbabbdc0f3ef6cbb47500b40ae235c1258b741a/src/mongo/db/commands/run_aggregate.cpp#L807
[runAggregate()]: https://github.com/mongodb/mongo/blob/dbbabbdc0f3ef6cbb47500b40ae235c1258b741a/src/mongo/db/commands/run_aggregate.cpp#L998
[rewriting a count command into aggregate command]: https://github.com/10gen/mongo/blob/4d35137259c93d1bc99db139ef86da105bd3358a/src/mongo/db/commands/count_cmd.cpp#L236
[views]: https://www.mongodb.com/docs/manual/core/views/

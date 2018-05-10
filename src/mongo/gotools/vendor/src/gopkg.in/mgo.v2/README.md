The MongoDB driver for Go
-------------------------

Please go to [http://labix.org/mgo](http://labix.org/mgo) for all project details.

## Testing

Tests require custom orchestration.  Install
[daemontools](https://cr.yp.to/daemontools.html) as a prerequisite and make
sure mongod and mongos are in your path.  To start the orchestration:

    $ export PATH=/path/to/mongodb/bin:$PATH
    $ make startdb

To stop the orchestration:

    $ make stopdb

Run all tests like this (`gocheck.v` turns on verbose output):

    $ go test -gocheck.v

To run a specific test, use the `gocheck.f` flag:

    $ go test -gocheck.v -gocheck.f TestFindAndModifyBug997828

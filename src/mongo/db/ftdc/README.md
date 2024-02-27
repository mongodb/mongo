# FTDC

## Table of Contents

-   [High Level Overview](#high-level-overview)

## High Level Overview

FTDC stands for Full-Time Diagnostic Data Capture. FTDC is used to capture data about the mongod and
mongos processes and the system that a mongo process is running on.

From a top down view, An
[`FTDCController`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.h)
object lives as a decoration on the service context. It is registered
[`here`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_server.cpp#L55). The
`FTDCController` is initialized from the mongod and mongos main functions, which call
[`startMongoDFTDC`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_mongod.h)
and
[`startMongoSFTDC`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_mongos.h)
respectively. The FTDC controller owns a collection of collector objects that gather system and
process information for the controller. These sets of collector objects are stored in a
[`FTDCCollectorCollection`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/collector.h#L77-L116)
object, allowing all the data to be collected through one call to collect on the
`FTDCCollectorCollection`. There are two sets of `FTDCCollectorCollection` objects on the
controller:
[\_periodicCollectors](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.h#L200-L201)
that collects data at a specified time interval, and
[\_rotateCollectors](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.h#L207-L208)
that collects one set of data every time a file is created.

At specified time intervals, the FTDC Controller calls collect on the `_periodicCollectors`
collection. To collect the data, most collectors run existing commands via
[`CommandHelpers::runCommandDirectly`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/commands.h#L219-L224)
which is abstracted into `FTDCSimpleInternalCommandCollector`. Some process data is gathered via
custom implementations of `FTDCCollectorInterface`. After gathering the data, the FTDC Controller
[writes](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/controller.cpp#L251-L252)
the data out as described below using an
[`FTDCFileManager`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.h).

Most collectors are of the class
[`FTDCSimpleInternalCommandCollector`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_server.cpp#L173).
The
[`FTDCServerStatusCommandCollector`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/ftdc_server.cpp#L195).
has a very specific query that it needs to run to get the server information. For example, it does
not collect sharding information from `serverStatus` because sharding could trigger issues with
compression efficiency of FTDC. The system stats are collected by `LinuxSystemMetricsCollector` via
`/proc` on Linux and `WindowsSystemMetricsCollector` via Windows perf counters on Windows.

The
[`FTDCFileManager`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.h)
is responsible for rotating files, managing disk space, and recovering files after a crash. When it
gets a hold of the data to be written to disk, it initially writes the data to an
[`FTDCCompressor`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/compressor.h)
object. The
[`FTDCCompressor`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/compressor.h)
compresses the data object and appends it to a chunk to be written out. If the chunk is full or the
BSON Schema has changed, the compressor indicates that the chunk needs to be written out to disk
immediately. Otherwise, the compressor indicates that it can store more data.

If the compressor can store more data there are two possibilities. If the data in the compressor has
reached a certain threshold, the
[`FTDCFileWriter`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_writer.h)
will decide to write out the unwritten section of data to an interim file. The goal of the interim
file is to flush data out in smaller intervals between archives to ensure the most up-to-date data
is recorded before a crash. The interim file also provides a chance for the regular files to have
maximum sized and compressed chunks. If not, nothing is written to outside of the compressor. If the
compressor indicates that the chunk needs to be written out immediately, data is flushed out of the
compressor into the archive file. The interim file is erased and written over, and the compressor
resets to take in new values.

The
[`FTDCFileManager`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.h)
also decides when to rotate the archive files. When the file gets too large, the manager deletes the
reference to the old file and starts writing immediately to the new file by calling
[rotate](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/ftdc/file_manager.cpp#L304-L324).

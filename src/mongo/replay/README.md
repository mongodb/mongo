## MONGOR

### The mongo replay client

MongoR is the mongo replay client. MongoR accepts a recording file, and replays the commands recorded against a different mongo instance.
In doing so, commands and queries can be replayed against different mongodb instances targetting different versions.
The ultimate goal is to evaluate the performances of different versions of mongodb.
In order to accomplish this, the replay client processes to the list of commands received and synchronize execution of the commands per session.
Each session is essentially an active connection, in which a set of commands are recorded.
In a single recording file there can be multiple sessions, for each sessions the replay client spawns a different execution thread.

### Recording Format

MongoR uses a recording file in order to replay the commands executed by a particular session or a set of sessions. Each recorded command is stored in binary format as per the folliwing data structure:

```cpp
struct TrafficRecordingPacket {
        const uint8_t eventType       // type of event (start/stop recording per session/connection)
        const uint64_t id;           // id of the session/connection
        const std::string session;  // ip addresses, not used by mongoR
        const uint64_t offset;     // timepoint of this recording packet. Some ref point from the beginning of the recording
        const uint64_t order;     // order of the query in the recording, for the session/connection recorded
        const Message message;   // bson doc representing the mongodb command (the query itself)
    };
```

- "eventType" => the type of the command. "0" is a normal command to replay, "1" is used for marking a start recording event and "2" a stop recording.
- "id" => this is the session id, the unique identifier used for marking each connection seen by mongodb. MongoR assigns an unique thread to each connection seen in the recording file. It leaves the burden of scheduling these threads to the OS, such as it has likely happened during recording.
- "offset" => each message carries some information about the time in which it was executed on the recording node. The list of commands is just a sequential list of messages. But in reality most of these messages could have been executed at different moments, and what we try to accomplish is to respect as much as possible the timing of these messages, sleeping the execution thread in case this is needed. Offset gives us a time point, a reference for the command associated.
- "order" => is the order in which the command has been executed on the recording host.
- "message" => this is the actual mongo db command. That's it. All MongoR does is to grab the stream of bytes and create a new internal mongodb message, to forward towards the instance used for simulating the original mongod/s instance.

### Command Replaying

Replaying a command is extremely simple. All we need to do is to provide 2 things: - Recording file of the instance we registered. - MongoURI of the instance we intend to replay the commands against.
MongoR is capable of reading the recording file, put it in memory and execute the commands one after another. That's it. For each session a new thread will be scheduled and all the commands associated to that particular session, will be executed one after the other, in respect of their "offset", stored in the recording file.
The MongoURI must be specified in order to target a valid shadow instance. The recording file and mongo uri can either be passed as argument, or simply put inside a configuration file.
In case of multiple recording files and multiple mongo uris, MongoR will handle each single replaying indipendently. Spinning up a session manager for each recording. Replaying the commands associated to each recording file accordingly to their connection id and targeting the right shadow instance.

### Session Handling

MongoR tries to replay all the connections and queries recorded in the recording file as precisely as possible. Meaning that MongoR is actually trying to simulate the same scheduling of the several connections recorded in the file that has been provided as input.
Each connection is generically called session by mongoR, in each session there can be an arbitrary number of queries. All the queries are run in the same order in which they are recorded. In order to comply with possible timeouts and generally with gaps that may or may not be present in the recording, mongoR will normalize all the times in respect of the current execution time, and execute the queries in respect of their original timestamp.
Multiple connections/sessions can overlap inside a recording. As matter of fact in a single recording file there can be hundreds of connections. MongoR does not make any promise about the order in which each connection will be scheduled, but guarantees that each session is treated independently from the others, in order to so, it schedules a separate thread for each recorded session. In this way, the scheduling of each single session is left to the operating system, which should do the best possible work in order to allocate the resources available in the machine.

## MongoR Design

MongoR design is quite simple. There are essentially 3 main parts:

- Traffic reader => process the recording
- Session Handling => replay the commands and simulate the execution
- Record execution stats => just collect responses and store them alongside execution time

MongoR will maintain ordering of requests within a session, but does not enforce relative ordering between sessions. Requests will be submitted as close to the target time as possible, but a overrunning operation can delay later requests for the same session, but other sessions will not be delayed to maintain any relative ordering.

### Classes

- `ConfigHandler` => handles the configuration parameters and/or the configuration file. Files: `config_handler.{cpp|h}`.
- `RecordingReader` => handles the recording reading and parsing. Files: `recording_reader.{cpp|h}`.
- `ReplayClient` => handles the spawning of several session handlers (one for each pair `<recording file,uri>`). It is the main entrypoint of mongoR. Files: `replay_client.{cpp|h}`.
- `ReplayCommandExecutor` => handles the mongodb connection process, the actual initialization of the mongoR client connection. Then exposes a simple set of utilities for connecting, disconnecting and executing bson commands. Files: `replay_command_executor.{cpp|h}`.
- `ReplayCommand` => handles each bson command and wraps it inside a simple abstraction that exposes a set of helpers for extracting quickly the information needed for each command (e.g is a start record event, etc). Files: ` replayCommand.{cpp|h}`.
- `SessionHandler` => handles all the processing required withing a single session. Everytime a packet is processed, the session id is passed down to the session handler in order to create a new SessionSimulator. There will be a SessionSimulator for each connection/session recorded. Files: `session_handler.{cpp|h}`.
- `SessionSimulator` => handles the replaying of the commands associated to a specific session/connection. Each `SessionSimulator` contains a `SessionScheduler` who is responsible of adding a background thread for processing all the commands concurrently. Files: `session_handler.{cpp|h}`.

### Compile MongoR and MongoR-Tests

For compiling MongoR executable all it is needed is bazel.

`bazel build install-mongor`

For launching mongor all you need is a recording file and the URI on the mongodb shadow instance where all the recording commands must be replayed against.

`./mongor -i <traffic input file> -t <mongod/s uri>`

alternative a configuration file can be specified:

`	./mongor -c <JSON config file> `

where the config file is:

```
{
  recordings:
  [
    {
      "path": "recording_path1",
      "uri": "uri1"
    },
    {
      "path": "recording_path2",
      "uri": "uri2"
    }
  ]
}
```

If you are developing mongor, then you might want to run the tests. In order to do so. From within the mongor folder `<mongo_folder>/src/mongo/replay` just run `bazel build mongor-test`.
There are several suites that can be used. To list all the suites `mongor-test --list`.

### Replaying stats

During command replaying MongoR stores the information about the execution itself. It does so saving the perf data inside a binary file, storing all the information in little endian. For each command executed we record:

```cpp
struct PerformancePacket {
    uint64_t session;
    uint64_t messageId;
    int64_t time;
    uint64_t ncount;
};
```

- `session` => represent the unique id, which indicates in which session/connection the command was executed.
- `messageId` => is the original id of the command in the session just replayed (field `order` in `TrafficRecordingPacket`). This is useful for locating the original command in the recording.
- `time` => is the actual execution time that mongor recorded for running the command. MongoR does not use explain or other query utilities for extracting this value. It just times the command execution.
- `number of documents returned (ncount)` => list of documents returned after the query execution. Useful for comparing recording and replaying results.

In case of multiple batches in the cursor returned after a `find` or `aggregate`, the total number of documents is the size of the first batch. `getMore` commands are not issued to navigate the cursor, unless explicitely recorded in the recording file.

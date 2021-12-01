/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TraceLoggingGraph_h
#define TraceLoggingGraph_h

#include "mozilla/MemoryReporting.h"

#include "js/TypeDecls.h"
#include "vm/MutexIDs.h"
#include "vm/TraceLoggingTypes.h"

/*
 * The output of a tracelogging session is saved in /tmp/tl-data.json.
 * The format of that file is a JS array per tracelogger (=thread), with a map
 * containing:
 *  - dict:   Name of the file containing a json table with the log text.
 *            All other files only contain a index to this table when logging.
 *  - events: Name of the file containing a flat list of log events saved
 *            in binary format.
 *            (64bit: Time Stamp Counter, 32bit index to dict)
 *  - tree:   Name of the file containing the events with duration. The content
 *            is already in a tree data structure. This is also saved in a
 *            binary file.
 *  - treeFormat: The format used to encode the tree. By default "64,64,31,1,32".
 *                There are currently no other formats to save the tree.
 *     - 64,64,31,1,32 signifies how many bytes are used for the different
 *       parts of the tree.
 *       => 64 bits: Time Stamp Counter of start of event.
 *       => 64 bits: Time Stamp Counter of end of event.
 *       => 31 bits: Index to dict file containing the log text.
 *       =>  1 bit:  Boolean signifying if this entry has children.
 *                   When true, the child can be found just right after this entry.
 *       => 32 bits: Containing the ID of the next event on the same depth
 *                   or 0 if there isn't an event on the same depth anymore.
 *
 *        /-> The position in the file. Id is this divided by size of entry.
 *        |   So in this case this would be 1 (192bits per entry).
 *        |                              /-> Indicates there are children. The
 *        |                              |   first child is located at current
 *        |                              |   ID + 1. So 1 + 1 in this case: 2.
 *        |                              |   Or 0x00180 in the tree file.
 *        |                              | /-> Next event on the same depth is
 *        |                              | |    located at 4. So 0x00300 in the
 *        |                              | |    tree file.
 *       0x0000C0: [start, end, dictId, 1, 4]
 *
 *
 *       Example:
 *                          0x0: [start, end, dictId, 1, 0]
 *                                        |
 *                      /----------------------------------\
 *                      |                                  |
 *       0xC0: [start, end, dictId, 0, 2]      0x180 [start, end, dictId, 1, 0]
 *                                                      |
 *                                  /----------------------------------\
 *                                  |                                  |
 *         0x240: [start, end, dictId, 0, 4]    0x300 [start, end, dictId, 0, 0]
 */

namespace js {
void DestroyTraceLoggerGraphState();
size_t SizeOfTraceLogGraphState(mozilla::MallocSizeOf mallocSizeOf);
} // namespace js

class TraceLoggerGraphState
{
    uint32_t numLoggers;
    uint32_t pid_;

    // File pointer to the "tl-data.json" file. (Explained above).
    FILE* out;

#ifdef DEBUG
    bool initialized;
#endif

  public:
    js::Mutex lock;

  public:
    TraceLoggerGraphState()
      : numLoggers(0)
      , pid_(0)
      , out(nullptr)
#ifdef DEBUG
      , initialized(false)
#endif
      , lock(js::mutexid::TraceLoggerGraphState)
    {}

    bool init();
    ~TraceLoggerGraphState();

    uint32_t nextLoggerId();
    uint32_t pid() { return pid_; }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
    }
};

class TraceLoggerGraph
{
    // The layout of the tree in memory and in the log file. Readable by JS
    // using TypedArrays.
    struct TreeEntry {
        uint64_t start_;
        uint64_t stop_;
        union {
            struct {
                uint32_t textId_: 31;
                uint32_t hasChildren_: 1;
            } s;
            uint32_t value_;
        } u;
        uint32_t nextId_;

        TreeEntry(uint64_t start, uint64_t stop, uint32_t textId, bool hasChildren,
                  uint32_t nextId)
        {
            start_ = start;
            stop_ = stop;
            u.s.textId_ = textId;
            u.s.hasChildren_ = hasChildren;
            nextId_ = nextId;
        }
        TreeEntry()
        { }
        uint64_t start() {
            return start_;
        }
        uint64_t stop() {
            return stop_;
        }
        uint32_t textId() {
            return u.s.textId_;
        }
        bool hasChildren() {
            return u.s.hasChildren_;
        }
        uint32_t nextId() {
            return nextId_;
        }
        void setStart(uint64_t start) {
            start_ = start;
        }
        void setStop(uint64_t stop) {
            stop_ = stop;
        }
        void setTextId(uint32_t textId) {
            MOZ_ASSERT(textId < uint32_t(1 << 31));
            u.s.textId_ = textId;
        }
        void setHasChildren(bool hasChildren) {
            u.s.hasChildren_ = hasChildren;
        }
        void setNextId(uint32_t nextId) {
            nextId_ = nextId;
        }
    };

    // Helper structure for keeping track of the current entries in
    // the tree. Pushed by `start(id)`, popped by `stop(id)`. The active flag
    // is used to know if a subtree doesn't need to get logged.
    struct StackEntry {
        uint32_t treeId_;
        uint32_t lastChildId_;
        struct {
            uint32_t textId_: 31;
            uint32_t active_: 1;
        } s;
        StackEntry(uint32_t treeId, uint32_t lastChildId, bool active = true)
          : treeId_(treeId), lastChildId_(lastChildId)
        {
            s.textId_ = 0;
            s.active_ = active;
        }
        uint32_t treeId() {
            return treeId_;
        }
        uint32_t lastChildId() {
            return lastChildId_;
        }
        uint32_t textId() {
            return s.textId_;
        }
        bool active() {
            return s.active_;
        }
        void setTreeId(uint32_t treeId) {
            treeId_ = treeId;
        }
        void setLastChildId(uint32_t lastChildId) {
            lastChildId_ = lastChildId;
        }
        void setTextId(uint32_t textId) {
            MOZ_ASSERT(textId < uint32_t(1<<31));
            s.textId_ = textId;
        }
        void setActive(bool active) {
            s.active_ = active;
        }
    };

  public:
    TraceLoggerGraph() {}
    ~TraceLoggerGraph();

    bool init(uint64_t timestamp);

    // Link a textId with a particular text.
    void addTextId(uint32_t id, const char* text);

    // Create a tree out of all the given events.
    void log(ContinuousSpace<EventEntry>& events);

    static size_t treeSizeFlushLimit() {
        // Allow tree size to grow to 100MB.
        return 100 * 1024 * 1024 / sizeof(TreeEntry);
    }

    uint32_t nextTextId() { return nextTextId_; }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  private:
    bool failed = false;
    bool enabled = false;
    uint32_t nextTextId_ = 0;

    FILE* dictFile = nullptr;
    FILE* treeFile = nullptr;
    FILE* eventFile = nullptr;

    ContinuousSpace<TreeEntry> tree;
    ContinuousSpace<StackEntry> stack;
    uint32_t treeOffset = 0;

    // Helper functions that convert a TreeEntry in different endianness
    // in place.
    void entryToBigEndian(TreeEntry* entry);
    void entryToSystemEndian(TreeEntry* entry);

    // Helper functions to get/save a tree from file.
    bool getTreeEntry(uint32_t treeId, TreeEntry* entry);
    bool saveTreeEntry(uint32_t treeId, TreeEntry* entry);

    // Return the first StackEntry that is active.
    StackEntry& getActiveAncestor();

    // This contains the meat of startEvent, except the test for enough space,
    // the test if tracelogger is enabled and the timestamp computation.
    void startEvent(uint32_t id, uint64_t timestamp);
    bool startEventInternal(uint32_t id, uint64_t timestamp);

    // Update functions that can adjust the items in the tree,
    // both in memory or already written to disk.
    bool updateHasChildren(uint32_t treeId, bool hasChildren = true);
    bool updateNextId(uint32_t treeId, uint32_t nextId);
    bool updateStop(uint32_t treeId, uint64_t timestamp);

    // Flush the tree.
    bool flush();

    // Stop a tree event.
    void stopEvent(uint32_t id, uint64_t timestamp);
    void stopEvent(uint64_t timestamp);

    // Log an (non-tree) event.
    void logTimestamp(uint32_t id, uint64_t timestamp);

    // Disable logging and forcefully report all not yet stopped tree events
    // as stopped.
    void disable(uint64_t timestamp);
};

#endif /* TraceLoggingGraph_h */

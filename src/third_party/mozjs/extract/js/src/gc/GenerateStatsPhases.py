# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# flake8: noqa: F821

# Generate graph structures for GC statistics recording.
#
# Stats phases are nested and form a directed acyclic graph starting
# from a set of root phases. Importantly, a phase may appear under more
# than one parent phase.
#
# For example, the following arrangement is possible:
#
#            +---+
#            | A |
#            +---+
#              |
#      +-------+-------+
#      |       |       |
#      v       v       v
#    +---+   +---+   +---+
#    | B |   | C |   | D |
#    +---+   +---+   +---+
#              |       |
#              +---+---+
#                  |
#                  v
#                +---+
#                | E |
#                +---+
#
# This graph is expanded into a tree (or really a forest) and phases
# with multiple parents are duplicated.
#
# For example, the input example above would be expanded to:
#
#            +---+
#            | A |
#            +---+
#              |
#      +-------+-------+
#      |       |       |
#      v       v       v
#    +---+   +---+   +---+
#    | B |   | C |   | D |
#    +---+   +---+   +---+
#              |       |
#              v       v
#            +---+   +---+
#            | E |   | E'|
#            +---+   +---+

# NOTE: If you add new phases here the current next phase kind number can be
# found at the end of js/src/gc/StatsPhasesGenerated.inc

import collections
import re


class PhaseKind:
    def __init__(self, name, descr, bucket, children=[]):
        self.name = name
        self.descr = descr
        # For telemetry
        self.bucket = bucket
        self.children = children


AllPhaseKinds = []
PhaseKindsByName = dict()


def addPhaseKind(name, descr, bucket, children=[]):
    assert name not in PhaseKindsByName
    phaseKind = PhaseKind(name, descr, bucket, children)
    AllPhaseKinds.append(phaseKind)
    PhaseKindsByName[name] = phaseKind
    return phaseKind


def getPhaseKind(name):
    return PhaseKindsByName[name]


PhaseKindGraphRoots = [
    addPhaseKind("MUTATOR", "Mutator Running", 0),
    addPhaseKind("GC_BEGIN", "Begin Callback", 1),
    addPhaseKind(
        "EVICT_NURSERY_FOR_MAJOR_GC",
        "Evict Nursery For Major GC",
        70,
        [
            addPhaseKind(
                "MARK_ROOTS",
                "Mark Roots",
                48,
                [
                    addPhaseKind("MARK_CCWS", "Mark Cross Compartment Wrappers", 50),
                    addPhaseKind("MARK_STACK", "Mark C and JS stacks", 51),
                    addPhaseKind("MARK_RUNTIME_DATA", "Mark Runtime-wide Data", 52),
                    addPhaseKind("MARK_EMBEDDING", "Mark Embedding", 53),
                ],
            )
        ],
    ),
    addPhaseKind("WAIT_BACKGROUND_THREAD", "Wait Background Thread", 2),
    addPhaseKind(
        "PREPARE",
        "Prepare For Collection",
        69,
        [
            addPhaseKind("UNMARK", "Unmark", 7),
            addPhaseKind("UNMARK_WEAKMAPS", "Unmark WeakMaps", 76),
            addPhaseKind("MARK_DISCARD_CODE", "Mark Discard Code", 3),
            addPhaseKind("RELAZIFY_FUNCTIONS", "Relazify Functions", 4),
            addPhaseKind("PURGE", "Purge", 5),
            addPhaseKind("PURGE_PROP_MAP_TABLES", "Purge PropMapTables", 60),
            addPhaseKind("PURGE_SOURCE_URLS", "Purge Source URLs", 73),
            addPhaseKind("JOIN_PARALLEL_TASKS", "Join Parallel Tasks", 67),
        ],
    ),
    addPhaseKind(
        "MARK",
        "Mark",
        6,
        [
            getPhaseKind("MARK_ROOTS"),
            addPhaseKind("MARK_DELAYED", "Mark Delayed", 8),
            addPhaseKind(
                "MARK_WEAK",
                "Mark Weak",
                13,
                [
                    getPhaseKind("MARK_DELAYED"),
                    addPhaseKind("MARK_GRAY_WEAK", "Mark Gray and Weak", 16),
                ],
            ),
            addPhaseKind("MARK_INCOMING_GRAY", "Mark Incoming Gray Pointers", 14),
            addPhaseKind("MARK_GRAY", "Mark Gray", 15),
            addPhaseKind(
                "PARALLEL_MARK",
                "Parallel marking",
                78,
                [
                    getPhaseKind("JOIN_PARALLEL_TASKS"),
                    # The following are only used for parallel phase times:
                    addPhaseKind("PARALLEL_MARK_MARK", "Parallel marking work", 79),
                    addPhaseKind("PARALLEL_MARK_WAIT", "Waiting for work", 80),
                    addPhaseKind(
                        "PARALLEL_MARK_OTHER", "Parallel marking overhead", 82
                    ),
                ],
            ),
        ],
    ),
    addPhaseKind(
        "SWEEP",
        "Sweep",
        9,
        [
            getPhaseKind("MARK"),
            addPhaseKind(
                "FINALIZE_START",
                "Finalize Start Callbacks",
                17,
                [
                    addPhaseKind("WEAK_ZONES_CALLBACK", "Per-Slice Weak Callback", 57),
                    addPhaseKind(
                        "WEAK_COMPARTMENT_CALLBACK", "Per-Compartment Weak Callback", 58
                    ),
                ],
            ),
            addPhaseKind("UPDATE_ATOMS_BITMAP", "Sweep Atoms Bitmap", 68),
            addPhaseKind("SWEEP_ATOMS_TABLE", "Sweep Atoms Table", 18),
            addPhaseKind(
                "SWEEP_COMPARTMENTS",
                "Sweep Compartments",
                20,
                [
                    addPhaseKind("SWEEP_DISCARD_CODE", "Sweep Discard Code", 21),
                    addPhaseKind("SWEEP_INNER_VIEWS", "Sweep Inner Views", 22),
                    addPhaseKind(
                        "SWEEP_CC_WRAPPER", "Sweep Cross Compartment Wrappers", 23
                    ),
                    addPhaseKind("SWEEP_BASE_SHAPE", "Sweep Base Shapes", 24),
                    addPhaseKind("SWEEP_INITIAL_SHAPE", "Sweep Initial Shapes", 25),
                    addPhaseKind("SWEEP_REGEXP", "Sweep Regexps", 28),
                    addPhaseKind("SWEEP_COMPRESSION", "Sweep Compression Tasks", 62),
                    addPhaseKind("SWEEP_WEAKMAPS", "Sweep WeakMaps", 63),
                    addPhaseKind("SWEEP_UNIQUEIDS", "Sweep Unique IDs", 64),
                    addPhaseKind("SWEEP_WEAK_POINTERS", "Sweep Weak Pointers", 81),
                    addPhaseKind(
                        "SWEEP_FINALIZATION_OBSERVERS",
                        "Sweep FinalizationRegistries and WeakRefs",
                        74,
                    ),
                    addPhaseKind("SWEEP_JIT_DATA", "Sweep JIT Data", 65),
                    addPhaseKind("SWEEP_WEAK_CACHES", "Sweep Weak Caches", 66),
                    addPhaseKind("SWEEP_MISC", "Sweep Miscellaneous", 29),
                    getPhaseKind("JOIN_PARALLEL_TASKS"),
                ],
            ),
            addPhaseKind("FINALIZE_OBJECT", "Finalize Objects", 33),
            addPhaseKind("FINALIZE_NON_OBJECT", "Finalize Non-objects", 34),
            addPhaseKind("SWEEP_PROP_MAP", "Sweep PropMap Tree", 77),
            addPhaseKind("FINALIZE_END", "Finalize End Callback", 38),
            addPhaseKind("DESTROY", "Deallocate", 39),
            getPhaseKind("JOIN_PARALLEL_TASKS"),
            addPhaseKind("FIND_DEAD_COMPARTMENTS", "Find Dead Compartments", 54),
        ],
    ),
    addPhaseKind(
        "COMPACT",
        "Compact",
        40,
        [
            addPhaseKind("COMPACT_MOVE", "Compact Move", 41),
            addPhaseKind(
                "COMPACT_UPDATE",
                "Compact Update",
                42,
                [
                    getPhaseKind("MARK_ROOTS"),
                    addPhaseKind("COMPACT_UPDATE_CELLS", "Compact Update Cells", 43),
                    getPhaseKind("JOIN_PARALLEL_TASKS"),
                ],
            ),
        ],
    ),
    addPhaseKind("DECOMMIT", "Decommit", 72),
    addPhaseKind("GC_END", "End Callback", 44),
    addPhaseKind(
        "MINOR_GC",
        "All Minor GCs",
        45,
        [
            getPhaseKind("MARK_ROOTS"),
        ],
    ),
    addPhaseKind(
        "EVICT_NURSERY",
        "Minor GCs to Evict Nursery",
        46,
        [
            getPhaseKind("MARK_ROOTS"),
        ],
    ),
    addPhaseKind(
        "TRACE_HEAP",
        "Trace Heap",
        47,
        [
            getPhaseKind("MARK_ROOTS"),
        ],
    ),
]


class Phase:
    # Expand the DAG into a tree, duplicating phases which have more than
    # one parent.
    def __init__(self, phaseKind, parent):
        self.phaseKind = phaseKind
        self.parent = parent
        self.depth = parent.depth + 1 if parent else 0
        self.children = []
        self.nextSibling = None
        self.nextInPhaseKind = None

        self.path = re.sub(r"\W+", "_", phaseKind.name.lower())
        if parent is not None:
            self.path = parent.path + "." + self.path


def expandPhases():
    phases = []
    phasesForKind = collections.defaultdict(list)

    def traverse(phaseKind, parent):
        ep = Phase(phaseKind, parent)
        phases.append(ep)

        # Update list of expanded phases for this phase kind.
        if phasesForKind[phaseKind]:
            phasesForKind[phaseKind][-1].nextInPhaseKind = ep
        phasesForKind[phaseKind].append(ep)

        # Recurse over children.
        for child in phaseKind.children:
            child_ep = traverse(child, ep)
            if ep.children:
                ep.children[-1].nextSibling = child_ep
            ep.children.append(child_ep)
        return ep

    for phaseKind in PhaseKindGraphRoots:
        traverse(phaseKind, None)

    return phases, phasesForKind


AllPhases, PhasesForPhaseKind = expandPhases()

# Name phases based on phase kind name and index if there are multiple phases
# corresponding to a single phase kind.

for phaseKind in AllPhaseKinds:
    phases = PhasesForPhaseKind[phaseKind]
    if len(phases) == 1:
        phases[0].name = "%s" % phaseKind.name
    else:
        for index, phase in enumerate(phases):
            phase.name = "%s_%d" % (phaseKind.name, index + 1)

# Find the maximum phase nesting.
MaxPhaseNesting = max(phase.depth for phase in AllPhases) + 1

# And the maximum bucket number.
MaxBucket = max(kind.bucket for kind in AllPhaseKinds)

# Generate code.


def writeList(out, items):
    if items:
        out.write(",\n".join("  " + item for item in items) + "\n")


def writeEnumClass(out, name, type, items, extraItems):
    items = ["FIRST"] + list(items) + ["LIMIT"] + list(extraItems)
    items[1] += " = " + items[0]
    out.write("enum class %s : %s {\n" % (name, type))
    writeList(out, items)
    out.write("};\n")


def generateHeader(out):
    #
    # Generate PhaseKind enum.
    #
    phaseKindNames = map(lambda phaseKind: phaseKind.name, AllPhaseKinds)
    extraPhaseKinds = [
        "NONE = LIMIT",
        "EXPLICIT_SUSPENSION = LIMIT",
        "IMPLICIT_SUSPENSION",
    ]
    writeEnumClass(out, "PhaseKind", "uint8_t", phaseKindNames, extraPhaseKinds)
    out.write("\n")

    #
    # Generate Phase enum.
    #
    phaseNames = map(lambda phase: phase.name, AllPhases)
    extraPhases = ["NONE = LIMIT", "EXPLICIT_SUSPENSION = LIMIT", "IMPLICIT_SUSPENSION"]
    writeEnumClass(out, "Phase", "uint8_t", phaseNames, extraPhases)
    out.write("\n")

    #
    # Generate MAX_PHASE_NESTING constant.
    #
    out.write("static const size_t MAX_PHASE_NESTING = %d;\n" % MaxPhaseNesting)


def generateCpp(out):
    #
    # Generate the PhaseKindInfo table.
    #
    out.write("static constexpr PhaseKindTable phaseKinds = {\n")
    for phaseKind in AllPhaseKinds:
        phase = PhasesForPhaseKind[phaseKind][0]
        out.write(
            '    /* PhaseKind::%s */ PhaseKindInfo { Phase::%s, %d, "%s" },\n'
            % (phaseKind.name, phase.name, phaseKind.bucket, phaseKind.name)
        )
    out.write("};\n")
    out.write("\n")

    #
    # Generate the PhaseInfo tree.
    #
    def name(phase):
        return "Phase::" + phase.name if phase else "Phase::NONE"

    out.write("static constexpr PhaseTable phases = {\n")
    for phase in AllPhases:
        firstChild = phase.children[0] if phase.children else None
        phaseKind = phase.phaseKind
        out.write(
            '    /* %s */ PhaseInfo { %s, %s, %s, %s, PhaseKind::%s, %d, "%s", "%s" },\n'
            % (  # NOQA: E501
                name(phase),
                name(phase.parent),
                name(firstChild),
                name(phase.nextSibling),
                name(phase.nextInPhaseKind),
                phaseKind.name,
                phase.depth,
                phaseKind.descr,
                phase.path,
            )
        )
    out.write("};\n")

    #
    # Print in a comment the next available phase kind number.
    #
    out.write("// The next available phase kind number is: %d\n" % (MaxBucket + 1))

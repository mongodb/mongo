/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.regex.*;
import java.util.stream.Collectors;

/**
 * Java program to estimate the memory usage of ICU objects (bug 1585536).
 *
 * It computes for each Intl constructor the amount of allocated memory. We're
 * currently using the maximum memory ("max" in the output) to estimate the
 * memory consumption of ICU objects.
 *
 * Insert before {@code JS_InitWithFailureDiagnostic} in "js.cpp":
 * 
 * <pre>
 * <code>
 * JS_SetICUMemoryFunctions(
 *     [](const void*, size_t size) {
 *       void* ptr = malloc(size);
 *       if (ptr) {
 *         printf("  alloc: %p -> %zu\n", ptr, size);
 *       }
 *       return ptr;
 *     },
 *     [](const void*, void* p, size_t size) {
 *       void* ptr = realloc(p, size);
 *       if (p) {
 *         printf("  realloc: %p -> %p -> %zu\n", p, ptr, size);
 *       } else {
 *         printf("  alloc: %p -> %zu\n", ptr, size);
 *       }
 *       return ptr;
 *     },
 *     [](const void*, void* p) {
 *       if (p) {
 *         printf("  free: %p\n", p);
 *       }
 *       free(p);
 *     });
 * </code>
 * </pre>
 * 
 * Run this script with:
 * {@code java --enable-preview --source=14 IcuMemoryUsage.java $MOZ_JS_SHELL}.
 */
@SuppressWarnings("preview")
public class IcuMemoryUsage {
    private enum Phase {
        None, Create, Init, Destroy, Collect, Quit
    }

    private static final class Memory {
        private Phase phase = Phase.None;
        private HashMap<Long, Map.Entry<Phase, Long>> allocations = new HashMap<>();
        private HashSet<Long> freed = new HashSet<>();
        private HashMap<Long, Map.Entry<Phase, Long>> completeAllocations = new HashMap<>();
        private int allocCount = 0;
        private ArrayList<Long> allocSizes = new ArrayList<>();

        void transition(Phase nextPhase) {
            assert phase.ordinal() + 1 == nextPhase.ordinal() || (phase == Phase.Collect && nextPhase == Phase.Create);
            phase = nextPhase;

            // Create a clean slate when starting a new create cycle or before termination.
            if (phase == Phase.Create || phase == Phase.Quit) {
                transferAllocations();
            }

            // Only measure the allocation size when creating the second object with the
            // same locale.
            if (phase == Phase.Collect && ++allocCount % 2 == 0) {
                long size = allocations.values().stream().map(Map.Entry::getValue).reduce(0L, (a, c) -> a + c);
                allocSizes.add(size);
            }
        }

        void transferAllocations() {
            completeAllocations.putAll(allocations);
            completeAllocations.keySet().removeAll(freed);
            allocations.clear();
            freed.clear();
        }

        void alloc(long ptr, long size) {
            allocations.put(ptr, Map.entry(phase, size));
        }

        void realloc(long oldPtr, long newPtr, long size) {
            free(oldPtr);
            allocations.put(newPtr, Map.entry(phase, size));
        }

        void free(long ptr) {
            if (allocations.remove(ptr) == null) {
                freed.add(ptr);
            }
        }

        LongSummaryStatistics statistics() {
            return allocSizes.stream().collect(Collectors.summarizingLong(Long::valueOf));
        }

        double percentile(double p) {
            var size = allocSizes.size();
            return allocSizes.stream().sorted().skip((long) ((size - 1) * p)).limit(2 - size % 2)
                    .mapToDouble(Long::doubleValue).average().getAsDouble();
        }

        long persistent() {
            return completeAllocations.values().stream().map(Map.Entry::getValue).reduce(0L, (a, c) -> a + c);
        }
    }

    private static long parseSize(Matcher m, int group) {
        return Long.parseLong(m.group(group), 10);
    }

    private static long parsePointer(Matcher m, int group) {
        return Long.parseLong(m.group(group), 16);
    }

    private static void measure(String exec, String constructor, String description, String initializer) throws IOException {
        var locales = Arrays.stream(Locale.getAvailableLocales()).map(Locale::toLanguageTag).sorted()
                .collect(Collectors.toUnmodifiableList());

        var pb = new ProcessBuilder(exec, "--file=-", "--", constructor, initializer,
                locales.stream().collect(Collectors.joining(",")));
        var process = pb.start();

        try (var writer = new BufferedWriter(
                new OutputStreamWriter(process.getOutputStream(), StandardCharsets.UTF_8))) {
            writer.write(sourceCode);
            writer.flush();
        }

        var memory = new Memory();

        try (var reader = new BufferedReader(new InputStreamReader(process.getInputStream()))) {
            var reAlloc = Pattern.compile("\\s+alloc: 0x(\\p{XDigit}+) -> (\\p{Digit}+)");
            var reRealloc = Pattern.compile("\\s+realloc: 0x(\\p{XDigit}+) -> 0x(\\p{XDigit}+) -> (\\p{Digit}+)");
            var reFree = Pattern.compile("\\s+free: 0x(\\p{XDigit}+)");

            String line;
            while ((line = reader.readLine()) != null) {
                Matcher m;
                if ((m = reAlloc.matcher(line)).matches()) {
                    var ptr = parsePointer(m, 1);
                    var size = parseSize(m, 2);
                    memory.alloc(ptr, size);
                } else if ((m = reRealloc.matcher(line)).matches()) {
                    var oldPtr = parsePointer(m, 1);
                    var newPtr = parsePointer(m, 2);
                    var size = parseSize(m, 3);
                    memory.realloc(oldPtr, newPtr, size);
                } else if ((m = reFree.matcher(line)).matches()) {
                    var ptr = parsePointer(m, 1);
                    memory.free(ptr);
                } else {
                    memory.transition(Phase.valueOf(line));
                }
            }
        }

        try (var errorReader = new BufferedReader(new InputStreamReader(process.getErrorStream()))) {
            String line;
            while ((line = errorReader.readLine()) != null) {
                System.err.println(line);
            }
        }

        var stats = memory.statistics();

        System.out.printf("%s%n", description);
        System.out.printf("  max: %d%n", stats.getMax());
        System.out.printf("  min: %d%n", stats.getMin());
        System.out.printf("  avg: %.0f%n", stats.getAverage());
        System.out.printf("  50p: %.0f%n", memory.percentile(0.50));
        System.out.printf("  75p: %.0f%n", memory.percentile(0.75));
        System.out.printf("  85p: %.0f%n", memory.percentile(0.85));
        System.out.printf("  95p: %.0f%n", memory.percentile(0.95));
        System.out.printf("  99p: %.0f%n", memory.percentile(0.99));
        System.out.printf("  mem: %d%n", memory.persistent());

        memory.transferAllocations();
        assert memory.persistent() == 0 : String.format("Leaked %d bytes", memory.persistent());
    }

    public static void main(String[] args) throws IOException {
        if (args.length == 0) {
            throw new RuntimeException("The first argument must point to the SpiderMonkey shell executable");
        }

        record Entry (String constructor, String description, String initializer) {
            public static Entry of(String constructor, String description, String initializer) {
                return new Entry(constructor, description, initializer);
            }

            public static Entry of(String constructor, String initializer) {
                return new Entry(constructor, constructor, initializer);
            }
        }

        var objects = new ArrayList<Entry>();
        objects.add(Entry.of("Collator", "o.compare('a', 'b')"));
        objects.add(Entry.of("DateTimeFormat", "DateTimeFormat (UDateFormat)", "o.format(0)"));
        objects.add(Entry.of("DateTimeFormat", "DateTimeFormat (UDateFormat+UDateIntervalFormat)",
                             "o.formatRange(0, 24*60*60*1000)"));
        objects.add(Entry.of("DisplayNames", "o.of('en')"));
        objects.add(Entry.of("ListFormat", "o.format(['a', 'b'])"));
        objects.add(Entry.of("NumberFormat", "o.format(0)"));
        objects.add(Entry.of("NumberFormat", "NumberFormat (UNumberRangeFormatter)",
                             "o.formatRange(0, 1000)"));
        objects.add(Entry.of("PluralRules", "o.select(0)"));
        objects.add(Entry.of("RelativeTimeFormat", "o.format(0, 'hour')"));

        for (var entry : objects) {
            measure(args[0], entry.constructor, entry.description, entry.initializer);
        }
    }

    private static final String sourceCode = """
const constructorName = scriptArgs[0];
const initializer = Function("o", scriptArgs[1]);
const locales = scriptArgs[2].split(",");

const extras = {};
addIntlExtras(extras);

for (let i = 0; i < locales.length; ++i) {
  // Loop twice in case the first time we create an object with a new locale
  // allocates additional memory when loading the locale data.
  for (let j = 0; j < 2; ++j) {
    let constructor = Intl[constructorName];
    let options = undefined;
    if (constructor === Intl.DisplayNames) {
      options = {type: "language"};
    }

    print("Create");
    let obj = new constructor(locales[i], options);

    print("Init");
    initializer(obj);

    print("Destroy");
    gc();
    gc();
    print("Collect");
  }
}

print("Quit");
quit();
""";
}

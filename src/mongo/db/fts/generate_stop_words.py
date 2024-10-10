import os
import sys


def read_stop_words(file_path):
    if not os.path.exists(file_path):
        print(f"Warning: File {file_path} does not exist. Skipping.")
        return []
    with open(file_path, "r", encoding="utf-8") as f:
        return [line.strip() for line in f if line.strip() and not line.strip().startswith("#")]


def generate(source, language_files):
    with open(source, "w", encoding="utf-8") as out:
        out.write("""
#include "mongo/db/fts/stop_words_list.h"

namespace mongo {
namespace fts {
  void loadStopWordMap(StringMap<std::set<std::string>>* m) {
    m->insert({
""")

        for l_file in language_files:
            language = l_file.rpartition("_")[2].partition(".")[0]
            words = read_stop_words(l_file)

            if words:
                out.write(f"      // {l_file}\n")
                out.write(f'      {{"{language}", {{\n')
                for word in words:
                    out.write(f'        "{word}",\n')
                out.write("      }},\n")

        out.write("""
    });
  }
} // namespace fts
} // namespace mongo
""")


if __name__ == "__main__":
    source_file = sys.argv[-1]
    language_files = sys.argv[1:-1]

    generate(source_file, language_files)

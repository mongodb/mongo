# Extracts the value for the "STABLE_CURRENT_YEAR" key in the stable status and writes it to a year file.
# Usage: python extract_certificate_generation_year.py <stable-status-file> <year-file>
import sys
from pathlib import Path


if __name__ == "__main__":
    status_file = Path(sys.argv[1])
    year_file = Path(sys.argv[2])
    lines = status_file.read_text(encoding="utf-8").splitlines()
    year = next(
        line.split()[1] for line in lines if line.split()[:1] == ["STABLE_CURRENT_YEAR"]
    ).strip()
    year_file.write_text(year + "\n", encoding="utf-8")

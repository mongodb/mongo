import sys

output = sys.argv[1]
links = sys.argv[2:]

with open(output, "w") as f:
    for link in links:
        f.write(f"{link}\n")

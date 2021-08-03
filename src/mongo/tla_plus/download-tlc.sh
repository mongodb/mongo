#!/bin/sh

# Downloads TLC, which is the model-checker for the TLA+ formal specifications in this directory.

echo "Downloading tla2tools.jar"
curl -fLO https://github.com/tlaplus/tlaplus/releases/download/v1.7.0/tla2tools.jar

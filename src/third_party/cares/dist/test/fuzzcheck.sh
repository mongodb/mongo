#!/bin/sh
set -e
# Check that all of the base fuzzing corpus parse without errors
./aresfuzz fuzzinput/*
./aresfuzzname fuzznames/*

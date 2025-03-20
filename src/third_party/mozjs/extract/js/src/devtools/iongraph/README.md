# iongraph

Visualizer for IonMonkey graphs using GraphViz.

## Usage

1. Make a debug build of SpiderMonkey.
2. Run with the env var `IONFLAGS=logs`. SpiderMonkey will dump observations about graph state into `/tmp/ion.json`.
3. Run `iongraph`. A PDF will be generated in your working directory for each function in the logs.

```
IONFLAGS=logs js -m mymodule.mjs
iongraph
```

For convenience, you can run the `iongraph js` command, which will invoke `js` and `iongraph` together. For example:

```
iongraph js -- -m mymodule.mjs
```

Arguments before the `--` separator are for `iongraph`, while args after `--` are for `js`.

## Graph properties

Blocks with green borders are loop headers. Blocks with red borders contain loop backedges (successor is a header). Blocks with dashed borders were created during critical edge splitting.

Instructions that are movable are blue. Guard instructions are underlined.

MResumePoints are placed as instructions, colored gray.

Edges from blocks ending with conditional branches are annotated with the truth value associated with each edge, given as '0' or '1'.

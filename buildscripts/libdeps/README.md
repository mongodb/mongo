# Libdeps Graph Analysis Tools

The Libdeps Graph analysis tools perform analysis and queries on graph representing the libdeps dependencies in the mongodb server builds.

## Generating the graph file

The scons build can create the graph files for analysis. To build the graphml file run the build with this minimal set of args required:

    python3 buildscripts/scons.py --link-model=dynamic --build-tools=next generate-libdeps-graph --linker=gold --modules=

The target `generate-libdeps-graph` has special meaning and will turn on extra build items to generate the graph. This target will build everything so that the graph is fully representative of the build. The graph file by default will be found at `build/opt/libdeps/libdeps.graphml` (where `build/opt` is the `$BUILD_DIR`).

## Command Line Tool

The Command Line tool will process a single graph file based off a list of input args. To see the full list of args run the command:

    python3 buildscripts/libdeps/gacli.py --help

By default it will performs some basic operations and print the output in human readable format:

    python3 buildscripts/libdeps/gacli.py --graph-file build/opt/libdeps/libdeps.graphml

Which will give an output similar to this:

    Loading graph data...Loaded!

    Graph built from git hash:
    19da729e2696bbf15d3a35c340281e4385069b88

    Graph Schema version:
    1

    Build invocation:
    "/usr/bin/python3.8" "buildscripts/scons.py" "--variables-files=etc/scons/mongodbtoolchain_stable_gcc.vars" "--dbg=on" "--opt=on" "--enable-free-mon=on" "--enable-http-client=on" "--cache=all" "--cache-dir=/home/ubuntu/scons-cache" "--install-action=hardlink" "--link-model=dynamic" "--build-tools=next" "--ssl" "--modules=enterprise" "CCACHE=ccache" "ICECC=icecc" "-j50" "generate-libdeps-graph"

    Nodes in Graph: 859
    Edges in Graph: 90843
    Direct Edges in Graph: 5808
    Transitive Edges in Graph: 85035
    Direct Public Edges in Graph: 3511
    Public Edges in Graph: 88546
    Private Edges in Graph: 2272
    Interface Edges in Graph: 25
    Shim Nodes in Graph: 20
    Program Nodes in Graph: 134
    Library Nodes in Graph: 725

    LibdepsLinter: PUBLIC libdeps that could be PRIVATE: 0

## Graph Visualizer Tool

The graph visualizer tools starts up a web service to provide a frontend GUI to navigating and examining the graph files. The Visualizer used a Python Flask backend and React Javascript frontend. You will need to install the libdeps requirements file to python to run the backend:

    python3 -m poetry install --no-root --sync -E libdeps

For installing the dependencies for the frontend, you will need node >= 12.0.0 and npm installed and in the PATH. To install the dependencies navigate to directory where package.json lives, and run:

    cd buildscripts/libdeps/graph_visualizer_web_stack && npm install

Alternatively if you are on linux, you can use the setup_node_env.sh script to automatically download node 12 and npm, setup the local environment and install the dependencies. Run the command:

    buildscripts/libdeps/graph_visualizer_web_stack/setup_node_end.sh install

Assuming you are on a remote workstation and using defaults, you will need to make ssh tunnels to the web service to access the service in your local browser. The frontend and backend both use a port (this case 3000 is the frontend and 5000 is the backend), and the default host is localhost, so you will need to open two tunnels so the frontend running in your local web browser can communicate with the backend. If you are using the default host and port the tunnel command will look like this:

    ssh -L 3000:localhost:3000 -L 5000:localhost:5000 ubuntu@workstation.hostname

Next we need to start the web service. It will require you to pass a directory where it will search for `.graphml` files which contain the graph data for various commits:

    python3 buildscripts/libdeps/graph_visualizer.py --graphml-dir build/opt/libdeps

The script will download nodejs, use npm to install all required packages, launch the backend and then build the optimized production frontend. You can supply the `--debug` argument to work in development load which allows real time updates as files are modified.

After the server has started up, it should notify you via the terminal that you can access it at http://localhost:3000 locally in your browser.

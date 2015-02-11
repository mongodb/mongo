# wtstats template

This directory contains the code to generate the `wtstats.html.template` file needed by the Python script `wtstats.py`.

The template is generated as a node.js "single page" application using several open source frameworks, see Dependencies below. The `wtstats.py` Python script parses stats files, transforms the data and inserts it as JSON into the template. The resulting HTML page is completely self-contained and can be viewed in any modern browser.

### Build Process

To build the template, you need `npm` (node package manager). `npm` is included in the Node.js packages and source, which you can get at [http://nodejs.org/download/](). On Ubuntu, you can also install `npm` via `sudo apt-get install npm`. On AWS Linux, you can install `npm` via `sudo yum install npm --enablerepo=epel`.

Once you have `npm` installed, follow these steps: 

1. change into the `./tools/template` directory (where this `README.md` is located)
2. run `npm install` to install all missing dependencies. 
3. run `npm run build` to build the template. 

The build script packs everything into a single HTML file and copies it to `./tools/template/wtstats.html.template`. Move the template file to the `./tools/` directory where `wtstats.py` is located. 

### Command Line Usage

Call the `wtstats.py` Python script located in the `./tools` directory with a stats file as argument. The `./tools/test` subfolder contains a small example stats file to test: 

```
python wtstats.py ./test/WiredTigerStat.fixture
```

The script will create `wtstats.html` in the working directory, which can be viewed with any modern browser (latest versions of Chrome, Safari, FireFox, Internet Explorer supported).

For more information about _wtstats_ usage, check out the [wtstats documentation](https://github.com/wiredtiger/wiredtiger/wiki/WiredTiger-statistics) on the WiredTiger wiki.


### UI Usage

When you open the generated html file (by default, this is called `wtstats.html`), you will see an empty page with some basic instructions in the middle, a sidebar containing grouped sections on the left, and buttons to toggle axis settings at the top right.

##### Toggling statistics

To look at specific statistics, open the respective section by clicking on the section heading or the little arrow on the right. For example, click on the word "connection" in the sidebar to reveal the _connection_ stats. Inside a group, click on any of the contained stats to toggle them on or off. You should see the stats graph appear in the main window. To toggle the entire group on or off, you can click on the circle next to the group heading. To look at one specific isolated statistic, you can shift-click on the stat. This will disable all but the chosen stat. Shift-clicking it again will return to the previous selection.

##### Filtering statistics

Depending on your WiredTiger settings, you may have gathered a large number of stats. Use the search bar above the stats groups to filter stats on keywords. For example, typing `mem` into the search bar will show all stats that have the substring "mem" in them. Press the "Clear" button to clear the filter again.

##### Navigating the View

Once some stats are showing in the main view panel, use the mouse to hover over the graphs. A cross-hair will appear highlighting the data point closest to the mouse cursor and showing the stat name and value (y-axis). A small gray label below the x-axis will additionally show the exact x-axis value for the current data point.

You can zoom into the graph via various mouse/trackpad gestures (some of which may not be supported by your input device):

- use the scroll wheel (mouse)
- use two-finger vertical scroll (touchpad)
- use pinch-to-zoom gesture (touchpad)
- use double-click to zoom in, shift+double-click to zoom out (mouse, touchpad)

If you zoom into a dataset far enough to see individual data points, they will be displayed as little circles along the graph line.

You can pan the graphs left/right by click-dragging the area horizontally. 

If your data contains a lot of sample points (more than pixels available in the current window), wtstats will sub-sample your data for efficiency reasons. When this happens, a yellow warning box will appear at the top next to the buttons. Due to sub-sampling, narrow spikes in a graph may not always be visible and you may experience some jitter when panning the data left/right. To ensure that all data points are rendered, widen your browser window or zoom into the data until the sub-sampling warning disappears.


##### Switching Axis Modes

wtstats lets you switch modes for both the x- and y-axis with the buttons at the top right. 

The x-axis button switches between relative and absolute time, with the default being relative time.

In "relative time" mode, every individual stat is assumed to start at time 0, and the x-axis displays the duration in seconds that have passed since the start. A useful use case for relative mode is when you record multiple stat files of the same data workload in succession, and want to compare them against each other. Relative mode will show the stat files overlapping, despite them being recorded at different times.

Absolute time plots the stats at their exact recording times. The x-axis shows date and time of the events. If stat files are recorded at different times, they appear in the graph at different intervals on the x-axis. This mode is most useful to correlate events in wtstats with other data sources, for example system profiling or monitoring data, based on their timestamp.

The y-axis button toggles between linear and logarithmic scaling mode, the default is linear. Linear mode is most useful if the visible stats have similar value ranges. However, if you compare stats of different magnitudes, for example "bytes in cache" vs. "files open", you can switch to "log-scale" mode to avoid having one graph dominate the entire range. The same is true for outliers in a single graph: if the outlier dominates the range, switch to log mode for better scaling. 


##### Display on Tablets and Phones

wtstats uses a responsive design that automatically adopts to the screen size. In the rare occasion where you need to look at a wtstats page on a smartphone or tablet device, the sidebar may be hidden to make the most of the limited screen size. To get to the side bar, click the wtstats logo at the top left and make your stats selection. Click again to hide the sidebar and go back to the graph content.



### Dependencies

- [ampersand.js](http://ampersandjs.com/), MV* framework, several modules, all under [MIT license][1]
- [Bootstrap](http://www.getbootstrap.com/), page layout and UI, [MIT license][2]
- [d3](http://d3js.org/), data bindings and visualization, [BSD license][3]
- [jQuery](http://jquery.com/), DOM manipulation, [MIT license][4]
- [lodash](https://lodash.com/), object/array transformation, [MIT license][5]
- [fontawesome](http://fontawesome.io/), icons, [OFL and MIT licenses][6]

For specific versions of these dependencies, see the [package.json](./package.json) file.  

[1]: https://github.com/AmpersandJS/ampersand-view/blob/master/LICENSE.md
[2]: https://github.com/twbs/bootstrap/blob/master/LICENSE
[3]: https://github.com/mbostock/d3/blob/master/LICENSE
[4]: https://github.com/jquery/jquery/blob/2.1.3/MIT-LICENSE.txt
[5]: https://github.com/lodash/lodash/blob/master/LICENSE.txt
[6]: http://fontawesome.io/license/


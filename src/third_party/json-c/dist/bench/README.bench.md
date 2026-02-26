
Benchmark tests for json-c

General strategy:
-------------------

* Identify "after" commit hash
    * Use provided directory
    * Use provided commit hash
    * Local changes in current working directory
    * ${cur_branch}
* Identify "before" commit hash, in order of preference
    * Use provided directory
    * Use provided commit hash
    * Use origin/${cur_branch}, if different from ${after_commit}
    * Use previous release

* If not using existing dir, clone to src-${after_commit}
    * or, checkout appropriate commit in existing src-${after_commit}
* Create build & install dirs for ${after_commit}
* Build & install ${after_commit}
* Compile benchmark programs against install-${after_commit}

* If not using existing dir, clone to src-${before_commit}
    * or, checkout appropriate commit in existing src-${before_commit}
* Create build & install dirs for ${before_commit}
* Build & install ${before_commit}
* Compile benchmark programs against install-${before_commit}

* Run benchmark in each location
* Compare results

heaptrack memory profiler
---------------------------

https://milianw.de/blog/heaptrack-a-heap-memory-profiler-for-linux.html


```
yum install libdwarf-devel elfutils boost-devel libunwind-devel

git clone git://anongit.kde.org/heaptrack
cd heaptrack
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_INSTALL_PREFIX=$HOME/heaptrack-install ..
make install
```


Issues
--------

* jc-bench.sh is incomplete.

* valgrind massif misreports "extra-heap" bytes?

    "json_parse -n canada.json" shows 38640 KB maxrss.

    Using valgrind --tool=massif, a large amount of memory is listed as
     wasted "extra-heap" bytes.  (~5.6MB)

    ```
    valgrind --tool=massif --massif-out-file=massif.out ./json_parse -n ~/canada.json
    ms_print massif.out
    ```


    Using heaptrack, and analyzing the histogram, only shows ~2.6MB
    ```
    heaptrack ./json_parse -n canada.json
    heaptrack --analyze heaptrack*gz -H histogram.out
    awk ' { s=$1; count=$2; ru=(int((s+ 15) / 16)) * 16; wasted = ((ru-s)*count); print s, count, ru-s, wasted; total=total+wasted} END { print "Total: ", total }' histogram.out
    ```

 With the (unreleased) arraylist trimming changes, maxrss reported by
  getrusage() goes down, but massif claims *more* total usage, and a HUGE 
  extra-heap amount (50% of total).


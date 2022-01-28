## Edit Distance Match Finder

```
/* This match finder leverages techniques used in file comparison algorithms
 * to find matches between a dictionary and a source file.
 * 
 * The original motivation for studying this approach was to try and optimize 
 * Zstandard for the use case of patching: the most common scenario being 
 * updating an existing software package with the next version. When patching,
 * the difference between the old version of the package and the new version 
 * is generally tiny (most of the new file will be identical to 
 * the old one). In more technical terms, the edit distance (the minimal number 
 * of changes required to take one sequence of bytes to another) between the 
 * files would be small relative to the size of the file. 
 * 
 * Various 'diffing' algorithms utilize this notion of edit distance and 
 * the corrensponding concept of a minimal edit script between two 
 * sequences to identify the regions within two files where they differ. 
 * The core algorithm used in this match finder is described in: 
 * 
 * "An O(ND) Difference Algorithm and its Variations", Eugene W. Myers,
 *    Algorithmica Vol. 1, 1986, pp. 251-266,
 *    <https://doi.org/10.1007/BF01840446>.
 * 
 * Additional algorithmic heuristics for speed improvement have also been included.
 * These we inspired from implementations of various regular and binary diffing 
 * algorithms such as GNU diff, bsdiff, and Xdelta. 
 * 
 * Note: after some experimentation, this approach proved to not provide enough 
 * utility to justify the additional CPU used in finding matches. The one area
 * where this approach consistenly outperforms Zstandard even on level 19 is 
 * when compressing small files (<10 KB) using a equally small dictionary that 
 * is very similar to the source file. For the use case that this was intended,
 * (large similar files) this approach by itself took 5-10X longer than zstd-19 and 
 * generally resulted in 2-3X larger files. The core advantage that zstd-19 has 
 * over this appraoch for match finding is the overlapping matches. This approach 
 * cannot find any. 
 * 
 * I'm leaving this in the contrib section in case this ever becomes interesting 
 * to explore again.
 * */
```

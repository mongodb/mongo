# TCMalloc Platforms

The TCMalloc code is supported on the following platforms. By "platforms", we
mean the union of operating system, architecture (e.g. little-endian vs.
big-endian), compiler, and standard library.

## Language Requirements

TCMalloc requires a code base that supports C++17 and our code is
C++17-compliant. C code is required to be compliant to C11.

We guarantee that our code will compile under the following compilation flags:

Linux:

*   gcc 9.2+, clang 9.0+: `-std=c++17`

(TL;DR; All code at this time must be built under C++17. We will update this
list if circumstances change.)

## Supported Platforms

The document below lists each platform, broken down by Operating System,
Architecture, Specific Compiler, and Standard Library implementation.

### Linux

**Supported**

<table width="80%">
  <col width="360">
  <col width="120">
  <tbody>
    <tr>
      <th>Operating System</th>
      <th>Endianness/Word Size</th>
      <th>Processor Architectures</th>
      <th>Compilers*</th>
      <th>Standard Libraries</th>
    </tr>
    <tr>
      <td>Linux</td>
      <td>little-endian, 64-bit</td>
      <td>x86, AArch64</td>
      <td>gcc 9.2+<br/>clang 9.0+</td>
      <td>libstdc++<br/>libc++</td>
    </tr>
  </tbody>
</table>

\* We test on gcc 9.2, though gcc versions (which support C++17) prior to that
release should also work.

**Best Effort**

<table width="80%">
  <col width="360">
  <col width="120">
  <tbody>
    <tr>
      <th>Operating System</th>
      <th>Endianness/Word Size</th>
      <th>Processor Architectures</th>
      <th>Compilers*</th>
      <th>Standard Libraries</th>
    </tr>
    <tr>
      <td>Linux</td>
      <td>little-endian, 64-bit</td>
      <td>PPC</td>
      <td>gcc 9.2+<br/>clang 9.0+</td>
      <td>libstdc++<br/>libc++</td>
    </tr>
  </tbody>
</table>

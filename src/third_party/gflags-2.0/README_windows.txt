You can compile this under Windows, if you want.  The solution file
(for VC 7.1 and later) is in this directory.

I've been told the following steps work to compile this under win64:
   1) Open the provided solution file
   2) Click on the Win32 target (on the right of Debug/Release)
   3) Choose Configuration Manager
   4) In Active Solution Platforms, choose New...
   5) In "Type of select the new platform", choose x64.
      In "Copy settings from:" choose Win32.
   6) Ok and then Close

I don't know very much about how to install DLLs on Windows, so you'll
have to figure out that part for yourself.  If you choose to just
re-use the existing .sln, make sure you set the IncludeDir's
appropriately!  Look at the properties for libgflags.dll.

You can also create a static library of gflags.  To do this, add
   /D GFLAGS_DLL_DECL= /D GFLAGS_DLL_DECLARE_FLAG= /D GFLAGS_DLL_DEFINE_FLAG=
to the compile line of every gflags .cc file.

If you create a static library that *uses* flags (that is, DEFINEs or
DECLAREs flags), you will need to add the following to every .cc file
that defines or declares a flag (it's safe to add them to every .cc
file in your project):
   /D GFLAGS_DLL_DECLARE_FLAG= /D GFLAGS_DLL_DEFINE_FLAG=

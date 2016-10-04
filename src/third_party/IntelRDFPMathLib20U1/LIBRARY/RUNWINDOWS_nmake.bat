echo "BEGIN BUILDING LIBRARY IN WINDOWS..."

del *.lib

call windowsbuild_nmake.bat -fmakefile.mak

echo "END BUILDING LIBRARY IN WINDOWS..."


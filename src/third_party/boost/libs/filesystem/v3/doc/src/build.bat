@echo off
del tr2.html >nul
mmp TARGET=TR2 source.html tr2.html
del reference.html >nul
mmp TARGET=BOOST source.html reference.html

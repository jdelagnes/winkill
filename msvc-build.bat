@echo off

rem clean
del *.obj *.exe

rem build
cl winkill.cpp /link advapi32.lib

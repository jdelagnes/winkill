@echo off

rem clean
del *.obj *.exe

rem build
mingw32-make winkill

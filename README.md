LatticeWM is a tiling window manager inspired by i3 and built for Windows Operating Systems. The long term goal is for it to implement all of the features that i3 has on linux.

I'm working on getting a proper makefile or build system in place but for now, if you want to compile and run this you can use this command:

g++ -std=c++17 -o tile_windows.exe main.cpp -lgdi32 -luser32 -lShcore -lpthread

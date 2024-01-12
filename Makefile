dwarffs: dwarffs.o
	g++ -g $^ -o $@ -lfuse -pthread -lnixutil -lnixmain -lnixstore

%.o: %.cc
	g++ -c -g -std=c++20 -Wall -Os $< -o $@

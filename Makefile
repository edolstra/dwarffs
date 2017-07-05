dwarffs: dwarffs.o
	g++ -g $^ -o $@ -lfuse -pthread -lnixutil -lnixmain

%.o: %.cc
	g++ -c -g -std=c++17 -Wall -Os $< -o $@

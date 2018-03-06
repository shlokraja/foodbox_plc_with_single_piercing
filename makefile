CC=gcc
CFLAGS=-Wno-write-strings -I.
LIBS=-lpthread -lplc -lplccip -lcurl -ljansson -lstdc++
LDIR=/usr/local/cti/lib
DEPS = PLCVariables.h PLCHandlerService.h
binaries = PLCHandler

%.o: %.cpp $(DEPS)
		$(CC) -c -o $@ $< $(CFLAGS)

plc: PLCFunctions.o PLCHandlerService.o
		$(CC) PLCFunctions.o PLCHandlerService.o -o PLCHandler $(CFLAGS) -L$(LDIR) $(LIBS)

clean:
		rm -f $(binaries) *.o

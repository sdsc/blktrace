CC	= gcc
CFLAGS	= -Wall -O2 -D_GNU_SOURCE
PROG	= blkparse blktrace
TRACE_LIBS = -lpthread

all: $(PROG)

blkparse: blkparse.o
blktrace: blktrace.o $(TRACE_LIBS)

clean:
	-rm -f *.o $(PROG)

CC	= gcc
CFLAGS	= -Wall -O2 -g -D_GNU_SOURCE
PROG	= blkparse blktrace
LIBS	= -lpthread

all: $(PROG)

blkparse: blkparse.o rbtree.o
blktrace: blktrace.o $(LIBS)

clean:
	-rm -f *.o $(PROG)

INSTALL = install
prefix = /usr/local
bindir = $(prefix)/bin

install: $(PROG)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(PROG) $(DESTDIR)$(bindir)


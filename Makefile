CC	= gcc
CFLAGS	= -Wall -O2 -g -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
PROGS	= blkparse blktrace verify_blkparse
LIBS	= -lpthread
SCRIPTS	= btrace

all: depend $(PROGS) $(SCRIPTS)

blkparse: blkparse.o blkparse_fmt.o rbtree.o act_mask.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^)

blktrace: blktrace.o act_mask.o $(LIBS)
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LIBS)

verify_blkparse: verify_blkparse.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^)

docs:
	$(MAKE) -C doc all

docsclean:
	$(MAKE) -C doc clean

depend:
	@$(CC) -MM $(CFLAGS) *.c 1> .depend

clean: docsclean
	-rm -f *.o $(PROGS) .depend

INSTALL = install
prefix = /usr/local
bindir = $(prefix)/bin

install: $(PROGS) $(SCRIPTS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(PROGS) $(SCRIPTS) $(DESTDIR)$(bindir)

ifneq ($(wildcard .depend),)
include .depend
endif

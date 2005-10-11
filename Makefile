CC	= gcc
CFLAGS	= -Wall -O2 -g
ALL_CFLAGS = $(CFLAGS) -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
PROGS	= blkparse blktrace verify_blkparse
LIBS	= -lpthread
SCRIPTS	= btrace

all: depend $(PROGS) $(SCRIPTS)

%.o: %.c
	$(CC) -o $*.o -c $(ALL_CFLAGS) $<

blkparse: blkparse.o blkparse_fmt.o rbtree.o act_mask.o
	$(CC) $(ALL_CFLAGS) -o $@ $(filter %.o,$^)

blktrace: blktrace.o act_mask.o $(LIBS)
	$(CC) $(ALL_CFLAGS) -o $@ $(filter %.o,$^) $(LIBS)

verify_blkparse: verify_blkparse.o
	$(CC) $(ALL_CFLAGS) -o $@ $(filter %.o,$^)

docs:
	$(MAKE) -C doc all

docsclean:
	$(MAKE) -C doc clean

depend:
	@$(CC) -MM $(ALL_CFLAGS) *.c 1> .depend

clean: docsclean
	-rm -f *.o $(PROGS) .depend

INSTALL = install
prefix = /usr/local
bindir = $(prefix)/bin

export prefix INSTALL

install: $(PROGS) $(SCRIPTS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(PROGS) $(SCRIPTS) $(DESTDIR)$(bindir)

ifneq ($(wildcard .depend),)
include .depend
endif

CC	= gcc
CFLAGS	= -Wall -O2 -g -D_GNU_SOURCE
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
	@$(CC) -MM $(CFLAGS) *.[ch] 1> .depend

clean: docsclean
	-rm -f *.o $(PROGS) .depend

INSTALL = install
prefix = /usr/local
bindir = $(prefix)/bin

install: $(PROGS) $(SCRIPTS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(PROGS) $(SCRIPTS) $(DESTDIR)$(bindir)

include .depend

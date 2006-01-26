CC	= gcc
CFLAGS	= -Wall -O2 -g
ALL_CFLAGS = $(CFLAGS) -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
PROGS	= blkparse blktrace verify_blkparse blkrawverify
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

blkrawverify: blkrawverify.o
	$(CC) $(ALL_CFLAGS) -o $@ $(filter %.o,$^)

docs:
	$(MAKE) -C doc all

docsclean:
	$(MAKE) -C doc clean

depend:
	@$(CC) -MM $(ALL_CFLAGS) *.c 1> .depend

INSTALL = install
prefix = /usr/local
bindir = $(prefix)/bin
RPMBUILD = rpmbuild
TAR = tar

export prefix INSTALL TAR

dist: btrace.spec
	git-tar-tree HEAD btrace-1.0 > btrace-1.0.tar
	@mkdir -p btrace-1.0
	@cp btrace.spec btrace-1.0
	$(TAR) rf btrace-1.0.tar btrace-1.0/btrace.spec
	@rm -rf btrace-1.0
	@bzip2 btrace-1.0.tar

rpm: dist
	$(RPMBUILD) -ta btrace-1.0.tar.bz2

clean: docsclean
	-rm -f *.o $(PROGS) .depend btrace-1.0.tar.bz2

install: $(PROGS) $(SCRIPTS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(PROGS) $(SCRIPTS) $(DESTDIR)$(bindir)

ifneq ($(wildcard .depend),)
include .depend
endif

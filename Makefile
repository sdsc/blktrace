CC	= gcc
CFLAGS	= -Wall -O2 -g -D_GNU_SOURCE
PROGS	= blkparse blktrace verify_blkparse
LIBS	= -lpthread
SCRIPTS	= btrace

DOCTMP	= blktrace.log blktrace.aux blktrace.dvi

all: $(PROGS) $(SCRIPTS)
docs: blktrace.pdf

blkparse: blkparse.o blkparse_fmt.o rbtree.o act_mask.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^)

blktrace: blktrace.o act_mask.o $(LIBS)
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LIBS)

verify_blkparse: verify_blkparse.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^)

blktrace.pdf:
	@latex blktrace.tex > /dev/null
	@latex blktrace.tex > /dev/null
	@dvipdfm -p a4 blktrace
	@rm -rf $(DOCTMP)

clean:
	-rm -f *.o $(PROGS) blktrace.pdf $(DOCTMP)

INSTALL = install
prefix = /usr/local
bindir = $(prefix)/bin

install: $(PROGS) $(SCRIPTS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(PROGS) $(SCRIPTS) $(DESTDIR)$(bindir)


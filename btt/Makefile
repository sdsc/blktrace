#
# OCFLAGS:
# 	COUNT_IOS	- Counts struct io's left at end
# 	DEBUG		- Various and sundy debug asserts
# 	NDEBUG		- Defined: no asserts, Undefined: asserts
#

CC	= gcc
INCS	= -I. -I..
OCFLAGS	= -UCOUNT_IOS -UDEBUG -DNDEBUG
XCFLAGS	= -Wall -W -g -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS	= $(INCS) -O2 $(XCFLAGS) $(OCFLAGS)

PROGS	= btt
LIBS	= $(PLIBS) $(ELIBS)
OBJS	= args.o bt_timeline.o devmap.o devs.o dip_rb.o iostat.o latency.o \
	  misc.o output.o proc.o seek.o trace.o trace_complete.o trace_im.o \
	  trace_issue.o trace_queue.o trace_remap.o trace_requeue.o rbtree.o \
	  mmap.o trace_plug.o bno_dump.o

all: depend $(PROGS)

rbtree.o: ../rbtree.c
	$(CC) -o $*.o -c $(CFLAGS) $<

depend:
	@$(CC) -MM $(CFLAGS) -I.. *.c 1> .depend

clean: docsclean
	-rm -f *.o $(PROGS) .depend

%.o: %.c
	$(CC) -o $*.o -c $(CFLAGS) $<

btt: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LIBS)

docs:
	$(MAKE) -C doc all

docsclean:
	$(MAKE) -C doc clean

ifneq ($(wildcard .depend),)
include .depend
endif

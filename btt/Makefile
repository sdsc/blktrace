CC	= gcc
CFLAGS	= -Wall -O2 -W -g
# CFLAGS	= -Wall -g -W -UDO_INLINE -DDEBUG
ALL_CFLAGS = $(CFLAGS) -I.. -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
PROGS	= btt
#ELIBS	= -lefence
#PLIBS	= -lpthread
LIBS	= $(PLIBS) $(ELIBS)
OBJS	= args.o bt_timeline.o devmap.o devs.o dip_rb.o iostat.o latency.o \
	  misc.o output.o proc.o seek.o trace.o trace_complete.o trace_im.o \
	  trace_issue.o trace_queue.o trace_remap.o trace_requeue.o rbtree.o \
	  mmap.o

all: depend $(PROGS)

rbtree.o: ../rbtree.c
	$(CC) -o $*.o -c $(ALL_CFLAGS) $<

depend:
	@$(CC) -MM $(ALL_CFLAGS) -I.. *.c 1> .depend

clean:
	-rm -f *.o $(PROGS) .depend

%.o: %.c
	$(CC) -o $*.o -c $(ALL_CFLAGS) $<

btt: $(OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $(filter %.o,$^) $(LIBS)

ifneq ($(wildcard .depend),)
include .depend
endif

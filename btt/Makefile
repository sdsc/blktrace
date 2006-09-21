CC	= gcc
MYFLAGS = -DLVM_REMAP_WORKAROUND
#CFLAGS	= -Wall -O2 -W -g
CFLAGS	= -Wall -g -W -UDO_INLINE -DDEBUG
ALL_CFLAGS = $(CFLAGS) -I.. -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
PROGS	= btt
#ELIBS	= -lefence
#PLIBS	= -lpthread
LIBS	= $(PLIBS) $(ELIBS)
OBJS	= bt_timeline.o args.o output.o proc.o trace.o misc.o devs.o \
	  traverse.o iofree.o devmap.o cylist.o seek.o iostat.o latency.o

all: depend $(PROGS)

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

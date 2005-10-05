#ifndef BLKTRACE_H
#define BLKTRACE_H

#include <stdio.h>
#include <byteswap.h>
#include <asm/types.h>
#include <asm/byteorder.h>
#include "blktrace_api.h"

#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)
#define MAJOR(dev)	((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)	((unsigned int) ((dev) & MINORMASK))

#define SECONDS(x) 		((unsigned long long)(x) / 1000000000)
#define NANO_SECONDS(x)		((unsigned long long)(x) % 1000000000)
#define DOUBLE_TO_NANO_ULL(d)	((unsigned long long)((d) * 1000000000))

#define min(a, b)	((a) < (b) ? (a) : (b))

typedef __u32 u32;
typedef __u8 u8;

struct io_stats {
	unsigned long qreads, qwrites, creads, cwrites, mreads, mwrites;
	unsigned long ireads, iwrites;
	unsigned long long qread_kb, qwrite_kb, cread_kb, cwrite_kb;
	unsigned long long iread_kb, iwrite_kb;
	unsigned long io_unplugs, timer_unplugs;
};

struct per_cpu_info {
	unsigned int cpu;
	unsigned int nelems;

	int fd;
	char fname[128];

	struct io_stats io_stats;
};

extern FILE *ofp;

#define CHECK_MAGIC(t)		(((t)->magic & 0xffffff00) == BLK_IO_TRACE_MAGIC)
#define SUPPORTED_VERSION	(0x05)

#if defined(__LITTLE_ENDIAN_BITFIELD)
#define be16_to_cpu(x)		__bswap_16(x)
#define be32_to_cpu(x)		__bswap_32(x)
#define be64_to_cpu(x)		__bswap_64(x)
#define cpu_to_be16(x)		__bswap_16(x)
#define cpu_to_be32(x)		__bswap_32(x)
#define cpu_to_be64(x)		__bswap_64(x)
#elif defined(__BIG_ENDIAN_BITFIELD)
#define be16_to_cpu(x)		(x)
#define be32_to_cpu(x)		(x)
#define be64_to_cpu(x)		(x)
#define cpu_to_be16(x)		(x)
#define cpu_to_be32(x)		(x)
#define cpu_to_be64(x)		(x)
#else
#error "Bad arch"
#endif

static inline int verify_trace(struct blk_io_trace *t)
{
	if (!CHECK_MAGIC(t)) {
		fprintf(stderr, "bad trace magic %x\n", t->magic);
		return 1;
	}
	if ((t->magic & 0xff) != SUPPORTED_VERSION) {
		fprintf(stderr, "unsupported trace version %x\n", 
			t->magic & 0xff);
		return 1;
	}

	return 0;
}

static inline void trace_to_be(struct blk_io_trace *t)
{
	t->magic	= cpu_to_be32(t->magic);
	t->sequence	= cpu_to_be32(t->sequence);
	t->time		= cpu_to_be64(t->time);
	t->sector	= cpu_to_be64(t->sector);
	t->bytes	= cpu_to_be32(t->bytes);
	t->action	= cpu_to_be32(t->action);
	t->pid		= cpu_to_be32(t->pid);
	t->cpu		= cpu_to_be32(t->cpu);
	t->error	= cpu_to_be16(t->error);
	t->pdu_len	= cpu_to_be16(t->pdu_len);
	t->device	= cpu_to_be32(t->device);
	/* t->comm is a string (endian neutral) */
}

static inline void trace_to_cpu(struct blk_io_trace *t)
{
	t->magic	= be32_to_cpu(t->magic);
	t->sequence	= be32_to_cpu(t->sequence);
	t->time		= be64_to_cpu(t->time);
	t->sector	= be64_to_cpu(t->sector);
	t->bytes	= be32_to_cpu(t->bytes);
	t->action	= be32_to_cpu(t->action);
	t->pid		= be32_to_cpu(t->pid);
	t->cpu		= be32_to_cpu(t->cpu);
	t->error	= be16_to_cpu(t->error);
	t->pdu_len	= be16_to_cpu(t->pdu_len);
	t->device	= be32_to_cpu(t->device);
	/* t->comm is a string (endian neutral) */
}

extern void set_all_format_specs(char *);
extern int add_format_spec(char *);
extern void process_fmt(char *, struct per_cpu_info *, struct blk_io_trace *,
			unsigned long long, int, unsigned char *);
extern int valid_act_opt(int);
extern int find_mask_map(char *);

#endif

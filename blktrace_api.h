#ifndef BLKTRACEAPI_H
#define BLKTRACEAPI_H

#include <asm/types.h>

/*
 * Trace categories
 */
enum {
	BLK_TC_READ	= 1 << 0,	/* reads */
	BLK_TC_WRITE	= 1 << 1,	/* writes */
	BLK_TC_BARRIER	= 1 << 2,	/* barrier */
	BLK_TC_SYNC	= 1 << 3,	/* barrier */
	BLK_TC_QUEUE	= 1 << 4,	/* queueing/merging */
	BLK_TC_REQUEUE	= 1 << 5,	/* requeueing */
	BLK_TC_ISSUE	= 1 << 6,	/* issue */
	BLK_TC_COMPLETE	= 1 << 7,	/* completions */
	BLK_TC_FS	= 1 << 8,	/* fs requests */
	BLK_TC_PC	= 1 << 9,	/* pc requests */

	BLK_TC_END	= 1 << 15,	/* only 16-bits, reminder */
};

#define BLK_TC_SHIFT		(16)
#define BLK_TC_ACT(act)		((act) << BLK_TC_SHIFT)

/*
 * Basic trace actions
 */
enum {
	__BLK_TA_QUEUE = 1,		/* queued */
	__BLK_TA_BACKMERGE,		/* back merged to existing rq */
	__BLK_TA_FRONTMERGE,		/* front merge to existing rq */
	__BLK_TA_GETRQ,			/* allocated new request */
	__BLK_TA_SLEEPRQ,		/* sleeping on rq allocation */
	__BLK_TA_REQUEUE,		/* request requeued */
	__BLK_TA_ISSUE,			/* sent to driver */
	__BLK_TA_COMPLETE,		/* completed by driver */
};

/*
 * Trace actions in full. Additionally, read or write is masked
 */
#define BLK_TA_QUEUE		(__BLK_TA_QUEUE | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_BACKMERGE	(__BLK_TA_BACKMERGE | BLK_TC_ACT(BLK_TC_QUEUE))
#define BLK_TA_FRONTMERGE	(__BLK_TA_FRONTMERGE | BLK_TC_ACT(BLK_TC_QUEUE))
#define	BLK_TA_GETRQ		(__BLK_TA_GETRQ | BLK_TC_ACT(BLK_TC_QUEUE))
#define	BLK_TA_SLEEPRQ		(__BLK_TA_SLEEPRQ | BLK_TC_ACT(BLK_TC_QUEUE))
#define	BLK_TA_REQUEUE		(__BLK_TA_REQUEUE | BLK_TC_ACT(BLK_TC_REQUEUE))
#define BLK_TA_ISSUE		(__BLK_TA_ISSUE | BLK_TC_ACT(BLK_TC_ISSUE))
#define BLK_TA_COMPLETE		(__BLK_TA_COMPLETE| BLK_TC_ACT(BLK_TC_COMPLETE))

#define BLK_IO_TRACE_MAGIC	0x65617400
#define BLK_IO_TRACE_VERSION	0x02

/*
 * The trace itself
 */
struct blk_io_trace {
	__u32 magic;		/* MAGIC << 8 | version */
	__u32 sequence;		/* event number */
	__u64 time;		/* in microseconds */
	__u64 sector;		/* disk offset */
	__u32 bytes;		/* transfer length */
	__u32 action;		/* what happened */
	__u32 pid;		/* who did it */
	__u16 error;		/* completion error */
	__u16 pdu_len;		/* length of data after this trace */
};

/*
 * User setup structure passed with BLKSTARTTRACE
 */
struct blk_user_trace_setup {
	char name[32];			/* output */
	__u16 act_mask;			/* input */
	__u32 buf_size;			/* input */
	__u32 buf_nr;			/* input */
};

#define BLKSTARTTRACE _IOWR(0x12,115,struct blk_user_trace_setup)
#define BLKSTOPTRACE _IO(0x12,116)

#endif

#ifndef BARRIER_H
#define BARRIER_H

#if defined(__ia64__)
#define store_barrier()         asm volatile ("mf" ::: "memory")
#elif defined(__x86_64__)
#define store_barrier()         asm volatile("sfence" ::: "memory")
#elif defined(__i386__)
#define store_barrier()         asm volatile ("": : :"memory")
#elif defined(__ppc__) || defined(__powerpc__)
#define store_barrier()         asm volatile ("eieio" : : : "memory")
#elif defined(__s390__) || defined(__s390x__)
#define store_barrier()         asm volatile ("bcr 15,0" : : : "memory")
#else
#error Define store_barrier() for your CPU
#endif

#endif

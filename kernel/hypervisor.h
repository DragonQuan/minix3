#ifndef HYPERVISOR_H
#define HYPERVISOR_H

#ifdef _TABLE
#undef EXTERN
#define EXTERN
#endif

#include <minix/config.h>
#include "config.h"
#include "proc.h"

/* virtual machine structure */ 
struct hyper_vm {

  /* The process table and pointers to process table slots. The pointers allow
   * faster access because now a process entry can be found by indexing the
   * pproc_addr array, while accessing an element i requires a multiplication
   * with sizeof(struct proc) to determine the address.
   * (from proc.h)
   */
  struct proc proc[NR_TASKS + NR_PROCS];   /* process table */
  struct proc *pproc_addr[NR_TASKS + NR_PROCS];
  struct proc *rdy_head[NR_SCHED_QUEUES]; /* ptrs to ready list headers */
  struct proc *rdy_tail[NR_SCHED_QUEUES]; /* ptrs to ready list tails */

  int who_e, who_p;        /* message source endpoint and proc (from glo.h) */

  /* VM (from glo.h) */
  phys_bytes vm_base;
  phys_bytes vm_size;
  phys_bytes vm_mem_high;
};

#define HYPER_NR_VMS     1     /* how many virtual machines */

/* every vm is a "domain" */
#define HYPER_VM(vm_id) (hyper_vm[(vm_id)])

/* HYPER_VM0 is the domain 0, the vm that has the systask and the real hardware */
#define HYPER_ID_VM0 0
#define HYPER_VM0 HYPER_VM(HYPER_ID_VM0)

/* Magic process table addresses. */
#define HYPER_BEG_PROC_ADDR(vm_id) (&(HYPER_VM(vm_id).proc[0]))
#define HYPER_BEG_USER_ADDR(vm_id) (&(HYPER_VM(vm_id).proc[NR_TASKS]))
#define HYPER_END_PROC_ADDR(vm_id) (&(HYPER_VM(vm_id).proc[NR_TASKS + NR_PROCS]))

#define hyper_cproc_addr(vm_id,n)     (&(HYPER_VM(vm_id).proc + NR_TASKS)[(n)])
#define hyper_proc_addr(vm_id,n)      (HYPER_VM(vm_id).pproc_addr + NR_TASKS)[(n)]

#define hyper_rdy_head(vm_id)      (HYPER_VM(vm_id).rdy_head)
#define hyper_rdy_tail(vm_id)      (HYPER_VM(vm_id).rdy_tail)

#endif /* HYPERVISOR_H */

/* The system call implemented in this file:
 *   m_type:	SYS_VM_SETBUF
 *
 * The parameters for this system call are:
 *    m4_l1:	Start of the buffer
 *    m4_l2:	Length of the buffer
 *    m4_l3:	End of main memory
 */
#include "../system.h"
#include "../hypervisor.h"

#define VM_DEBUG 0		/* enable/ disable debug output */

/*===========================================================================*
 *				do_vm_setbuf				     *
 *===========================================================================*/
PUBLIC int do_vm_setbuf(m_ptr)
message *m_ptr;			/* pointer to request message */
{
	HYPER_VM(0).vm_base= m_ptr->m4_l1;
	HYPER_VM(0).vm_size= m_ptr->m4_l2;
	HYPER_VM(0).vm_mem_high= m_ptr->m4_l3;

#if VM_DEBUG
	kprintf("do_vm_setbuf: got 0x%x @ 0x%x for 0x%x\n",
		HYPER_VM(0).vm_size, HYPER_VM(0).vm_base, HYPER_VM(0).vm_mem_high);
#endif

	return OK;
}

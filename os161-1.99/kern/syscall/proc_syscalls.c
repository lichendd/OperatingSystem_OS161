#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <synch.h>
#include <array.h>
#include <vfs.h>
#include <vm.h>
#include "opt-A2.h"
  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

#if OPT_A2
int sys_fork(struct trapframe *tf, pid_t * retval) {
  struct proc *child = proc_create_runprogram("child");
  if (child == NULL) {
    return -1;
  }

  struct addrspace *new_as = as_create();
  int result = as_copy(curproc->p_addrspace, &new_as);
  if (result != 0) {
    return -1;
  }
  spinlock_acquire(&child->p_lock);
  child->p_addrspace = new_as;
  spinlock_release(&child->p_lock);

  lock_acquire(curproc->lock);
  array_add(curproc->child, child, NULL);
  int * id = kmalloc(sizeof(int));
  * id = child->pid;
  array_add(curproc->ChildPid, id, NULL);
  int * s = kmalloc(sizeof (int));
  * s = -10086;
  array_add(curproc->ChildStatus, s, NULL); // -10086 represent the child is alive
  child->parent = curproc;
  lock_release(curproc->lock);
  struct trapframe * tf_c = kmalloc(sizeof (struct trapframe));
  tf_c = memcpy(tf_c, tf, sizeof(struct trapframe));
  thread_fork("child_thread", child, enter_forked_process, (void *) tf_c, 0);
  *retval = child->pid;
  return (0);
}

int sys_execv(userptr_t progname, userptr_t args_in) {
  // count the number of args
  char** args = (char**)args_in;
  int arg_size = 0;
  while(args[arg_size] != NULL) {
    arg_size ++;
  }
  // copy name to kernel
  char * programName = kmalloc(128 * sizeof(char));
  KASSERT(programName != NULL);
  int e = copyin(progname, programName, 128);
  KASSERT(e == 0);

  // copy args to kernel
  struct array * arguments = array_create();
  for (int i = 0; i < arg_size; i++) {
    char * element = kmalloc(sizeof(char) * 128);
    KASSERT(element != NULL);
    e = copyin((const_userptr_t) args[i], (void *) element, 128);
    array_add(arguments, element, NULL);
  }
  char * element = kmalloc(sizeof(char) * 128);
  KASSERT(element != NULL);
  array_add(arguments, NULL, NULL);

  //code from runprogram
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(programName, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
  // save the old address so that we can delete it later
	struct addrspace * old_addr = curproc_setas(as); 
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

  // copy to userstack
  vaddr_t userStack = stackptr;
  struct array * heapAddr = array_create();
  for (int i = 0; i < arg_size; i++) {
    char * tmp = kmalloc(sizeof(char) * 128);
    strcpy(tmp, (char*)array_get(arguments,i));
    array_add(heapAddr, tmp, NULL);
  }
  for (int i = 0; i < arg_size; i++) {
    userStack -= 128;
    copyout(array_get(heapAddr, arg_size-i-1), (userptr_t)userStack, 128);
  }
  userStack -= 128;
  copyout(NULL, (userptr_t)userStack, 128);
 
  for (int i = 0; i < arg_size; i++) {
    userStack -= 4;
    char ** t = (char**) userStack;
    *t = (char*)userStack + 128*(arg_size-i)+4*(1+i);
  }
	
  // delete the old address
  as_destroy(old_addr);
  kfree(programName);
  
	/* enter_new_process does not return. */
  enter_new_process(arg_size, (userptr_t)userStack, userStack, entrypoint); 
	panic("enter_new_process returned\n");
	return EINVAL;
}
#endif /* OPT_A2 */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  // (void)exitcode;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);

#if OPT_A2
  // check if I am a child
  struct proc* me = curproc;
  me = me;

  if (curproc->parent != NULL) {
    lock_acquire (curproc->parent->lock);
    // I am a child, need to tell my parent my exitcode. 
    struct proc * p = curproc->parent;
    for (int i = 0; i < (int)array_num(p->child);i++) {
      struct proc* curChild = (struct proc*)array_get(p->child,i);
      if (curChild!=NULL && curChild->pid == curproc->pid) {
        int * childExit = array_get(p->ChildStatus, i);
        * childExit = exitcode;
        // array_remove(p->child,i);
        array_set(p->child, i, NULL);
        break;
      }
    }
    lock_release(curproc->parent->lock);
    // A child also need to wake up its parent
    cv_signal(curproc->finish, curproc->lock);
  }


  // check if I am a parent
  lock_acquire(curproc->lock);

  // I am a parent, need to tell my children that I died :( 
  int size = array_num(curproc->child);
  for (int i = 0; i < size; i++) {
    struct proc * c = array_get(curproc->child, i);
    if ( c!= NULL) c->parent = NULL;
  }
  while (array_num(curproc->child)) {
    array_remove(curproc->child, 0);
  }

  // also need to free everything
  while (array_num(curproc->ChildPid)) {
    kfree(array_get(curproc->ChildPid,0));
    kfree(array_get(curproc->ChildStatus,0));
    array_remove(curproc->ChildPid,0);
    array_remove(curproc->ChildStatus,0);
  }

  lock_release(curproc->lock);

#endif

  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);
  
  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = curproc->pid;
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;
  
  struct proc* me = curproc;
  me = me;
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }

  int childStatus = -99; // the child status to be returned
  // go through curproc->child to see if the child proc is still running.
  lock_acquire(curproc->lock);
  
  int size = array_num(curproc->child);
  bool childAlive = false;
  struct proc * child;
  int curpid;
  for (int i = 0; i < size; i++) {
    curpid = *(int*)array_get(curproc->ChildPid,i);
    if (curpid == pid) {
      child = (struct proc *) array_get(curproc->child, i);
      if (child != NULL) {
        lock_acquire(child->lock);
        childAlive = true;
      }
      break;
    }
  }
  
  lock_release(curproc->lock);

  if (childAlive) {
    // the child is still running, wait till it finish
    cv_wait(child->finish, child->lock);
  }

  lock_acquire(curproc->lock);
  // the child proc has finished, ready to find the status.
  size = array_num(curproc->ChildPid);
  for (int i = 0; i < size; i++) {
    if (*(int*)array_get(curproc->ChildPid, i) == pid) {
      childStatus = *(int*) array_get(curproc->ChildStatus, i);
    }
  }
  lock_release(curproc->lock);

  exitstatus = _MKWAIT_EXIT(childStatus);
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

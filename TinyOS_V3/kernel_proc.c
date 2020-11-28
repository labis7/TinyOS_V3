
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;
  pcb->exitflag = 0;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  rlnode_init(& pcb->threads_list, NULL);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURTHREAD->owner_ptcb->task;
  int argl =  CURTHREAD->owner_ptcb->argl;
  void* args =  CURTHREAD->owner_ptcb->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  Mutex_Lock(&kernel_mutex);

  /* The new process PCB */
  newproc = acquire_PCB();
  newproc->exitflag = 0;

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  if(call != NULL) {
	  void* argsptr; 
	  if(args!=NULL) {
		  argsptr = malloc(argl);
		  memcpy(argsptr, args, argl);
	  }
	  else
		  argsptr=NULL;

	  TCB* mainthread = spawn_thread(newproc, start_main_thread);
	  PTCB* ptcb = (PTCB*) malloc(sizeof(PTCB));
	  ptcb->thread = mainthread;
	  ptcb->thread->owner_ptcb = ptcb;
	  ptcb->task = call;
	  ptcb->argl = argl;
	  ptcb->args = argsptr;
	  ptcb->joinable = 1;
	  ptcb->joincv = COND_INIT;
	  ptcb->interrupted = 0;
	  ptcb->done = 0;
	  rlnode_init(&(ptcb->ptcb_node), ptcb);
	  rlist_push_back(&(newproc->threads_list), &(ptcb->ptcb_node)); 	
	  newproc->main_thread=mainthread;
	
	  wakeup(mainthread);
  }


finish:
  Mutex_Unlock(&kernel_mutex);
  return get_pid(newproc);
}


/* System call */
Pid_t GetPid()
{
  return get_pid(CURPROC);
}


Pid_t GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{
  Mutex_Lock(& kernel_mutex);

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    Cond_Wait(& kernel_mutex, & parent->child_exit);
  
  cleanup_zombie(child, status);
  
finish:
  Mutex_Unlock(& kernel_mutex);
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;
  Mutex_Lock(&kernel_mutex);

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  if(is_rlist_empty(& parent->children_list)) {
    cpid = NOPROC;
    goto finish;
  }

  while(is_rlist_empty(& parent->exited_list)) {
    Cond_Wait(& kernel_mutex, & parent->child_exit);
  }

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

finish:
  Mutex_Unlock(& kernel_mutex);
  return cpid;
}


Pid_t WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void Exit(int exitval)
{
  /* Right here, we must check that we are not the boot task. If we are, 
     we must wait until all processes exit. */
  Mutex_Lock(&kernel_mutex);
  if(CURPROC->exitflag == 1){
	  Mutex_Unlock(&kernel_mutex);
	  ThreadExit(exitval);
  }
  else{
	  CURPROC->exitflag = 1;
	  Mutex_Unlock(&kernel_mutex);
  }

  Mutex_Lock(&kernel_mutex);
  if(CURTHREAD->owner_ptcb->joinable == 1){
	  ((TCB*)ThreadSelf())->owner_ptcb->exitval = exitval;
	  ((TCB*)ThreadSelf())->owner_ptcb->joinable = 0;
	  Cond_Broadcast(& ((TCB*)ThreadSelf())->owner_ptcb->joincv);
  }
  Mutex_Unlock(&kernel_mutex);

  if(GetPid()==1) {
    while(WaitChild(NOPROC,NULL)!=NOPROC);
  }

  while(1){
	  Mutex_Lock(&kernel_mutex);
	  rlnode* tmp1 = CURPROC->threads_list.next;
	  int c = 0;
	  while(tmp1 != & CURPROC->threads_list){
		if(tmp1->ptcb->done == 0 || tmp1->ptcb->thread == CURTHREAD){
			c++;
		}
		tmp1 = tmp1->next;
	  }
	  if(c==1){
		  break;
	  }
	  Mutex_Unlock(&kernel_mutex);
	  yield(SR);
  }  
  rlnode* tmp1 = CURPROC->threads_list.next;
  rlnode* tmp2;
  while(tmp1 != & CURPROC->threads_list){
	  tmp2 = tmp1->next;
	  if(tmp1->ptcb->done && tmp1->ptcb->thread != CURTHREAD){
		  if(tmp1->ptcb->thread == CURPROC->main_thread){
			  free(tmp1->ptcb->args);
			  tmp1->ptcb->args = NULL;
		  }
		  rlist_remove(tmp1);
		  free(tmp1->ptcb);
	  }
	  tmp1 = tmp2;
  }

  /* Now, we exit */

  /* Do all the other cleanup we want here, close files etc. */
  if(CURTHREAD == CURPROC->main_thread){
	  free(CURTHREAD->owner_ptcb->args);
	  CURTHREAD->owner_ptcb->args = NULL;
  }


  PCB *curproc = CURPROC;  /* cache for efficiency */

  
  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }

  /* Reparent any children of the exiting process to the 
     initial task */
  PCB* initpcb = get_pcb(1);
  while(!is_rlist_empty(& curproc->children_list)) {
    rlnode* child = rlist_pop_front(& curproc->children_list);
    child->pcb->parent = initpcb;
    rlist_push_front(& initpcb->children_list, child);
  }

  /* Add exited children to the initial task's exited list 
     and signal the initial task */
  if(!is_rlist_empty(& curproc->exited_list)) {
    rlist_append(& initpcb->exited_list, &curproc->exited_list);
    Cond_Broadcast(& initpcb->child_exit);
  }

  /* Put me into my parent's exited list */
  if(curproc->parent != NULL) {   /* Maybe this is init */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    Cond_Broadcast(& curproc->parent->child_exit);
  }

  /* Disconnect my main_thread */
  rlist_remove(& CURTHREAD->owner_ptcb->ptcb_node);
  free(CURTHREAD->owner_ptcb);

  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;
  curproc->exitval = exitval;

  /* Bye-bye cruel world */
  sleep_releasing(EXITED, & kernel_mutex, SR);
}



Fid_t OpenInfo()
{
	return NOFILE;
}


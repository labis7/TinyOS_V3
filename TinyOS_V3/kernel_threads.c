
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"


void start_thread_func();
/** 
 *   @brief Create a new thread in the current process.
 *     */
Tid_t CreateThread(Task task, int argl, void* args)
{
	Mutex_Lock(&kernel_mutex);
	TCB* newthread = spawn_thread(CURPROC, start_thread_func);
	PTCB* ptcb = (PTCB*) malloc(sizeof(PTCB));
	ptcb->thread = newthread;
	ptcb->thread->owner_ptcb = ptcb;
	ptcb->task = task;
	ptcb->argl = argl;
	ptcb->args = args;
	ptcb->joinable = 1;
	ptcb->interrupted = 0;
	ptcb->joiners = 0;
	ptcb->joincv = COND_INIT;
	ptcb->done = 0;
	rlnode_init(&(ptcb->ptcb_node), ptcb);
	rlist_push_back(&(CURPROC->threads_list), &(ptcb->ptcb_node)); 	
	wakeup(ptcb->thread);
	Mutex_Unlock(&kernel_mutex);
	return (Tid_t) newthread;
}

/**
 *   @brief Return the Tid of the current thread.
 *    */
Tid_t ThreadSelf()
{
	return (Tid_t) CURTHREAD;
}

/**
 *   @brief Join the given thread.
 *     */
int ThreadJoin(Tid_t tid, int* exitval)
{

	TCB* threadtojoin = (TCB*) tid;
	int val = 0;
	Mutex_Lock(&kernel_mutex);
	if(tid == NOTHREAD || tid == ThreadSelf()){
		val = -1;
		goto finish;
	}

	rlnode* tmp = CURPROC->threads_list.next;
	PTCB* owner_ptcb = NULL;
	while(tmp != & CURPROC->threads_list){
		if(tmp->ptcb->thread == threadtojoin){
			owner_ptcb = tmp->ptcb;
			break;
		}
		tmp = tmp->next;
	}
	if(owner_ptcb == NULL){
		val = -1;
		goto finish;
	}
	if(owner_ptcb->joinable==1 && owner_ptcb->done==1){
	       	goto acquireexitval;
	}if(owner_ptcb->joinable==0 && owner_ptcb->done==1){
		val = -1;
	       	goto finish;
	}
	++owner_ptcb->joiners;
	while(owner_ptcb->joinable==1 && owner_ptcb->done == 0){
	       	Cond_Wait(& kernel_mutex,& owner_ptcb->joincv);
	}
	if(owner_ptcb->joiners==0){
		val = -1;
		goto finish;
	}
	--owner_ptcb->joiners;
acquireexitval:
	if(exitval != NULL){
		*exitval = owner_ptcb->exitval;
	}
	if(owner_ptcb->joiners == 0){
		owner_ptcb->joinable = 0;	
	}
finish:		
	Mutex_Unlock(&kernel_mutex);
	return val;
}

/**
 *   @brief Detach the given thread.
 *     */
int ThreadDetach(Tid_t tid)
{
	Mutex_Lock(& kernel_mutex);
	PTCB* owner_ptcb = NULL;
	rlnode* tmp = CURPROC->threads_list.next;
	TCB* tcb = (TCB*) tid;
	while(tmp != & CURPROC->threads_list){
		if(tmp->ptcb->thread == tcb){
			owner_ptcb = tmp->ptcb;
			break;
		}
		tmp = tmp->next;
	}
	if(owner_ptcb == NULL){
		Mutex_Unlock(& kernel_mutex);
		return -1;
	}
	if(owner_ptcb->joinable){ 
		owner_ptcb->joinable = 0;
		owner_ptcb->joiners = 0;
		Cond_Broadcast(& tcb->owner_ptcb->joincv);
		Mutex_Unlock(& kernel_mutex);
		return 0;
	}
	Mutex_Unlock(& kernel_mutex);
	return -1;
}

/**
 *   @brief Terminate the current thread.
 *     */
void ThreadExit(int exitval)
{
	Mutex_Lock(& kernel_mutex);
	rlnode* tmp1 = CURPROC->threads_list.next;
	int c = 0;
	while(tmp1 != & CURPROC->threads_list){
		if(tmp1->ptcb->done == 0){
			c++;
		}
		tmp1 = tmp1->next;
	}
	if(c==1){
		Mutex_Unlock(&kernel_mutex);
		Exit(exitval);
	}
	if(CURTHREAD->owner_ptcb->joinable == 1){
		((TCB*)ThreadSelf())->owner_ptcb->exitval = exitval;
		Cond_Broadcast(& ((TCB*)ThreadSelf())->owner_ptcb->joincv);
	}
	((TCB*)ThreadSelf())->owner_ptcb->done = 1;
	sleep_releasing(EXITED, & kernel_mutex, SR);
}
/**
 *   @brief Awaken the thread, if it is sleeping.
 *
 *     This call will set the interrupt flag of the
 *       thread.
 *
 *         */
int ThreadInterrupt(Tid_t tid)
{
	Mutex_Lock(& kernel_mutex);
	PTCB* owner_ptcb = NULL;
	rlnode* tmp = CURPROC->threads_list.next;
	TCB* tcb = (TCB*) tid;
	while(tmp != & CURPROC->threads_list){
		if(tmp->ptcb->thread == tcb){
			owner_ptcb = tmp->ptcb;
			break;
		}
		tmp = tmp->next;
	}
	if(owner_ptcb == NULL){
		Mutex_Unlock(& kernel_mutex);
		return -1;
	}
	owner_ptcb->interrupted = 1;
	if(tcb->state == STOPPED){
		wakeup(tcb);
	}
	Mutex_Unlock(&kernel_mutex);
	return 0;
}


/**
 *   @brief Return the interrupt flag of the 
 *     current thread.
 *       */
int ThreadIsInterrupted()
{
	return CURTHREAD->owner_ptcb->interrupted;
}

/**
 *   @brief Clear the interrupt flag of the
 *     current thread.
 *       */
void ThreadClearInterrupt()
{
	Mutex_Lock(& kernel_mutex);
	((TCB*)ThreadSelf())->owner_ptcb->interrupted = 0;
	Mutex_Unlock(& kernel_mutex);
}



void start_thread_func()
{
	int exitval;

	Task call =  ((TCB*)ThreadSelf())->owner_ptcb->task;
	int argl = ((TCB*)ThreadSelf())->owner_ptcb->argl;
	void* args = ((TCB*)ThreadSelf())->owner_ptcb->args;

	exitval = call(argl,args);
	ThreadExit(exitval);

}


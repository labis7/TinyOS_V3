#include <stdio.h>
#include "unit_testing.h"
#include "util.h"
#include "tinyoslib.h"

void dummy_func(){
}

extern const Test all_my_tests;

BARE_TEST(mytest1, "This is a silly test")
{
	//initialize_processes();
	//PCB *pcb = pcb_freelist;
	//ASSERT(get_pid(pcb)==0);
	MSG("SUCCESS!\n");
}

/*BARE_TEST(mytest2,"myyyy test!"){
  initialize_scheduler();
  TCB* tcb=spawn_thread(NULL,dummy_func);
  sched_queue_add(tcb);
  ASSERT(!is_rlist_empty(&SCHED));
  }*/

TEST_SUITE(all_my_tests, "These are mine")
{
	&mytest1,
		// &mytest2,
		NULL
};

int main(int argc, char** argv)
{
	return register_test(&all_my_tests) ||
		run_program(argc, argv, &all_my_tests);
}


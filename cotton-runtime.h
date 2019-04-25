#include<functional>
namespace cotton
{
 	void init_tracing();
 	void init_replay();
	void find_and_execute_task();
	void *worker_routine(void *p);
	std::function<void()> grab_task_from_runtime();
	void push_task_to_runtime(std::function<void()> lambda);
}

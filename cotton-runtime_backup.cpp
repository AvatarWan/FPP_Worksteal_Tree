#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <functional>
#include <queue>
#include <time.h>
#include <sys/time.h>
#include <cstring>
#include <fstream>
#include <iostream>

namespace cotton
{

#define DEBUG false
pthread_mutex_t fin_lock=PTHREAD_MUTEX_INITIALIZER; //finish_counter lock if defined then use mutex_lock


volatile bool shutdown=true; //true-not running,false-running

int size=1;
pthread_t *threads;
int finish_counter=0;

pthread_key_t key;
int getKey()
{
	return 	*((int *)pthread_getspecific(key));
}
int getSize()
{
	return size;
}


//*********************************Tracing code**********************
//*******************************************************************
bool REPLAY = false;
bool TRACE = false;

class ContinuationHdr
{public:
	ContinuationHdr(const ContinuationHdr &t)
	{
		thisT = t.thisT;
		level = t.level;
		step = t.step;
	}
	std::function<void()> thisT;
	int level= -1; //this task's async level
	int step=0; //this continuation's step
	ContinuationHdr(std::function<void()> &t)
	{
		thisT = t;
		level = -1;
		step = 0;
	}
};


//Rather then creating new class for ReplayContinuationHeader i have created it inside Task itself
class Task : public ContinuationHdr
{
public:
	Task(const Task &t) : ContinuationHdr(t)
	{
		atFrontier = t.atFrontier;
	}
	Task(std::function<void()> &t) : ContinuationHdr(t)
	{
		atFrontier = false;
	}
	// This function is used to create dummy task for the root Task i.e. between start_finish and end_finish
	static Task *dummyTask()
	{
		std::function<void()> temp;
		Task *t = new Task(temp);
		t->level = 0;
		t->step = 0;
		return t;
	}

// # used for the replay part
	bool atFrontier = false;//could any of its children have been stolen, initially false
};


class Cont : public  ContinuationHdr
{

};

// #######################Sending and Reciving the Task between Workers Datatype
class PipeDeQueue
{

	class TaskList
	{
	private:
		long max_size;
		Task *list; 	//arraylist for deque
		double grow_rate=2.0;
	public:
		TaskList(): max_size(1000) {
			list= (Task *)malloc(sizeof(Task) * max_size);
		}
		~TaskList(){
			free(list);
		}
		long size()
		{
			return max_size;
		}
		Task *get(long i)
		{
			return &list[i % max_size];
		}
		void put(long i,Task &task)
		{
			list[i%max_size]=task;
		}
		void grow(long b,long t)
		{
			long new_max_size = max_size*grow_rate;
			Task *temp = (Task *)malloc(sizeof(Task)*new_max_size);
			for(long i=t;i<b;i++)
				temp[i % new_max_size]=list[i % max_size];
			max_size=new_max_size;
			free(list);
			list=temp;
		}
	};

	TaskList tasks;
	long top=0;
	long bottom=0;
	
	public:
		PipeDeQueue() {
			top=0;
			bottom=0; //unicluded last element
		}
		~PipeDeQueue() {}

		pthread_mutex_t tq_lock=PTHREAD_MUTEX_INITIALIZER;
	bool compareAndSwapTop(long oldV,long newV)
	{
		bool r;
		pthread_mutex_lock(&tq_lock);
		r= (oldV==top);
		if(r)
			top=newV;
		pthread_mutex_unlock(&tq_lock);
		return r;
	}


	//***********Push to Bottom********************
	void push(Task &task)
	{
		long b=bottom;
		long t=top;
		if((b-t) >= (tasks.size()-1) )	//TaskList is Full so grow it
		{
			printf("Regrowing Task List for %d\n",0);
			tasks.grow(b,t);
		}
		tasks.put(b,task);
		bottom = b+1;
	}
	//**********************************************

	//************Pop from top**********************
	Task *popTop()
	{
		long t = top;
		long b = bottom;
		if(b <= t) //TaskList is Empty
			return NULL;
		Task *task=tasks.get(t);

		if(!compareAndSwapTop(t,t+1))
			return NULL;	//here no need for synchronization handle as only one worker at one time(basically the one which is theif is executing it)

		return task;

	}
	//*************************************************


};

//


class WorkingPhaseInfo
{
public:
	int victim= -2;			//victim stolen from
	std::vector<int> stepStolen;	//step stolen at each level init should be -1
	std::vector<int> thieves;	//list of the thieves
	WorkingPhaseInfo(Task *c)
	{
		c->level = 0;	//@start of working phase with continuation c level of starting frame
	}

// #used for replay part
	int curr_theif=0;
	int getNextTheif()
	{
		if(curr_theif< thieves.size())
			return thieves[curr_theif++];
		printf("ERROR: no more theif\n");
		return -1;
	}
	bool isMoreTheif()
	{
		return curr_theif < thieves.size();
	}
protected:
	WorkingPhaseInfo(){}
};

class HelpFirstInfo :  public  WorkingPhaseInfo
{
public:
	std::vector<int> nTasksStolen;	//num. tasks stolen at each level, init to 0
	pthread_mutex_t nTasksStolen_lock=PTHREAD_MUTEX_INITIALIZER; //used in manipulation of nTasksStolen size and element manipulation between steal by other and async by itself


// #used for the replay part
	std::vector<int> childCount; // num children for current executing task
// #####
	HelpFirstInfo(Task *c) : WorkingPhaseInfo(c)
	{}
public:
	HelpFirstInfo()
	{}


	void nTasksStolenPush_Back(int v)
	{
		pthread_mutex_lock(&nTasksStolen_lock);
		nTasksStolen.push_back(v);
		pthread_mutex_unlock(&nTasksStolen_lock);
	}
	void nTasksStolenSet(int i,int val)
	{
		pthread_mutex_lock(&nTasksStolen_lock);
		nTasksStolen[i]=val;
		pthread_mutex_unlock(&nTasksStolen_lock);	
	}

public:
	bool validate()
	{
		int totalThief = 0;
		for(int i=0;i<nTasksStolen.size();i++)
			totalThief+=nTasksStolen[i];
		return totalThief==thieves.size();
	}
	void print()
	{
		printf("\t victim : %d\n",victim);
		printf("\t thieves :\n\t");
		for(int i=0;i<thieves.size();i++)
			printf("\t%d",thieves[i]);
		printf("\n\tnTasksStolen at each level :\n\t");
		for(int i=0;i<nTasksStolen.size();i++)
			printf("\t%d",nTasksStolen[i]);
		printf("\n\n");
			
	}
};


//These funtions are created to put the trace of the running code to a local file.

//This is just a helper funtion to write vector of any kind
template<typename T>
void writeVector(std::ostream &strm,const std::vector<T> &v)
{
	strm << v.size()<< std::endl;
	for(int i=0;i<v.size();i++)
		strm << v[i] << " ";
	strm << std::endl;
}
//This is just a helper funtion
template<typename T>
void readVector(std::istream &stm,std::vector<T> &v)
{
	int vsize;
	stm >> vsize;
	v.resize(vsize);
	for(int i=0;i<v.size();i++)
		stm >> v[i];
}
std::ostream& operator<<(std::ostream &strm, const HelpFirstInfo &a) {
	strm << a.victim<<std::endl;
	writeVector(strm,a.stepStolen);
	writeVector(strm,a.thieves);
	writeVector(strm,a.nTasksStolen);
	return strm;
}
std::istream& operator>> ( std::istream& stm, HelpFirstInfo &a )
{
	stm >> a.victim;
	readVector(stm,a.stepStolen);
	readVector(stm,a.thieves);
	readVector(stm,a.nTasksStolen);
	return stm;
}

class WorkingStateHdr
{
public:

	WorkingStateHdr()
	{

	}
	WorkingStateHdr(const WorkingStateHdr &t)
	{
		this->wpi = t.wpi;
	}
	//state of each working phase
	std::vector<HelpFirstInfo> wpi;

	void addWorkingPhase(int myrank,HelpFirstInfo &new_wpi);

	bool validate()
	{
		for(int i=0;i<wpi.size();i++)
			if(!wpi[i].validate())
				return false;
		return true;
	}

	void print()
	{
		for(int i=0;i<wpi.size();i++)
		{
			printf("workphase number %d\n",i);
			wpi[i].print();
		}
	}
};

class ReplayWorkingStateHdr : public WorkingStateHdr
{
public:
	//Copy Constructor
	ReplayWorkingStateHdr(const ReplayWorkingStateHdr &t) : WorkingStateHdr(t)
	{
		this->sendPipe = t.sendPipe;
		this->recPipe = t.recPipe;
		this->current_wpi = t.current_wpi;
		this->count_wpi = t.count_wpi;
	}

	HelpFirstInfo current_wpi;
	//pipeline for stolen tasks between workers
	std::vector<PipeDeQueue *>  sendPipe;
	std::vector<PipeDeQueue *> recPipe;

	int count_wpi=0;
	HelpFirstInfo getNextWPI()
	{
		return wpi[count_wpi++];
	}
	bool isMoreWPI()
	{
		return (count_wpi < wpi.size());
	}
	ReplayWorkingStateHdr() : WorkingStateHdr()
	{
		count_wpi=0;
		sendPipe.resize(size);
		recPipe.resize(size);
	}

	static void setPipeLines(ReplayWorkingStateHdr *replay_wsh)
	{
		for(int i=0;i<size;i++)
			for(int j=0;j<size;j++)
				if(i!=j)
				{
					printf("%d -- %d\n",i,j);
					//Send stolen task from i to j
					PipeDeQueue *temp = new PipeDeQueue();
					replay_wsh[i].sendPipe[j] = temp;
					replay_wsh[j].recPipe[i] = temp;
				}
				else{
					replay_wsh[i].sendPipe[j]=NULL;
					replay_wsh[j].recPipe[i]=replay_wsh[i].sendPipe[j];
				}
	}

	static ReplayWorkingStateHdr *getReplayWorkingStateHdr(WorkingStateHdr &w)
	{
		ReplayWorkingStateHdr *temp = new ReplayWorkingStateHdr();
		temp->wpi = w.wpi;
		return temp;
	}


};

std::ostream& operator<<(std::ostream &strm, const WorkingStateHdr &a) {
	writeVector<HelpFirstInfo>(strm,a.wpi);	
	return strm;
}
std::istream& operator>> ( std::istream& stm, WorkingStateHdr &a )
{
	readVector<HelpFirstInfo>(stm,a.wpi);
	return stm;
}
void writeWorkerStateHdrs(char *location,WorkingStateHdr *arr)
{
	std::ofstream file_obj;
	file_obj.open(location,std::ofstream::trunc);
	file_obj<<size<<std::endl;
	for(int i=0;i<size;i++)
	{
		file_obj<< arr[i];
	}	
	file_obj.close();
}
ReplayWorkingStateHdr *readReplayWorkerStateHdrs(char *location)
{
	std::ifstream file_obj;
	file_obj.open(location);
	int tempSize;
	file_obj>>tempSize;
	ReplayWorkingStateHdr *arr = new ReplayWorkingStateHdr[tempSize];
	for(int i=0;i<size;i++)
	{
		WorkingStateHdr temp;
		file_obj>> temp;
		arr[i] = *ReplayWorkingStateHdr::getReplayWorkingStateHdr(temp);
	}
	file_obj.close();
	return arr;
}


//size is actually NWORKERS

// Used for the tracing
WorkingStateHdr *wsh;	//one per worker and malloced at init_tracing
Task **executingTasks;

//Used for the replay
ReplayWorkingStateHdr *replay_wsh;

void init_executionTask()
{
	//This is keeping track of which worker executing which task currently
	executingTasks = (Task **) malloc(sizeof(Task *)*size);
	//*****Remember initialy size-1 rank worker with execute the root task i.e. start_finish task
	executingTasks[size-1] = Task::dummyTask(); // root task that work at start_finish

}

void init_replay()
{
	init_executionTask();
	executingTasks[size-1]->atFrontier = true;
	replay_wsh = readReplayWorkerStateHdrs("temp.trace");
	ReplayWorkingStateHdr::setPipeLines(replay_wsh);
		printf("****************Result after retracing************* \n");
		for(int i=0;i<size;i++)
		{
			printf("WORKER %d list of workphase\n",i);
			replay_wsh[i].print();
		}

	replay_wsh[size-1].current_wpi = replay_wsh[size-1].getNextWPI();

}

void init_tracing()
{
	init_executionTask();

	wsh = new WorkingStateHdr[size];
	HelpFirstInfo phase(executingTasks[size-1]);
	phase.victim = -1; //initiallize initial worker 1st workingphase and set its victim to -1 as it is not stolen from anyone
	wsh[size-1].wpi.push_back(phase);

}

int getTempMyFuck(int victim,int level)
{
	if(level>=replay_wsh[victim].current_wpi.nTasksStolen.size())
		return 0;
	else
		return replay_wsh[victim].current_wpi.nTasksStolen[level];

}


void markStolen(int victim,Task &t,HelpFirstInfo &current_wpi)
{
	int thief;
	//enqueue't' for sending to the next theif in current working phase info's thieves
	if(current_wpi.isMoreTheif())
		thief = current_wpi.getNextTheif();
	else{
		printf("ERROR : no more theif,for worker:%d ,count_wpi:%d ,last value of curr_theif:%d \n",victim,replay_wsh[victim].count_wpi-1,current_wpi.curr_theif);
		printf("ERROR : level:%d nChild:%d nTasksStolen:%d\n",t.level,current_wpi.childCount[t.level],getTempMyFuck(victim,t.level) );
	}
	(replay_wsh[victim].sendPipe[thief])->push(t);

	printf("%d can take from %d\n",thief,victim);
	//the one it is stealing will not be pushed into the dequeue of the victim
}







//********************Work-Stealing Aproch for thread-pool runtime data structure***************
//**********************************************************************************************


class DeQueue
{

	class TaskList
	{
	private:
		long max_size;
		Task *list; 	//arraylist for deque
		double grow_rate=2.0;
	public:
		TaskList(): max_size(1000) {
			list= (Task *)malloc(sizeof(Task) * max_size);
		}
		~TaskList(){
			free(list);
		}
		long size()
		{
			return max_size;
		}
		Task *get(long i)
		{
			return &list[i % max_size];
		}
		void put(long i,Task &task)
		{
			list[i%max_size]=task;
		}
		void grow(long b,long t)
		{
			long new_max_size = max_size*grow_rate;
			Task *temp = (Task *)malloc(sizeof(Task)*new_max_size);
			for(long i=t;i<b;i++)
				temp[i % new_max_size]=list[i % max_size];
			max_size=new_max_size;
			free(list);
			list=temp;
		}
	};
public:
	pthread_mutex_t tq_lock=PTHREAD_MUTEX_INITIALIZER; //task queue lock
private:
	TaskList tasks;
	long top=0;
	long bottom=0;
	
	public:
		DeQueue() {
			top=0;
			bottom=0; //unicluded last element
		}
		~DeQueue() {}

	//***********Push to Bottom********************
	void push(Task &task)
	{
		long b=bottom;
		long t=top;
		if((b-t) >= (tasks.size()-1) )	//TaskList is Full so grow it
		{
			printf("Regrowing Task List for %d\n",0);
			tasks.grow(b,t);
		}
		tasks.put(b,task);
		bottom = b+1;
		if(DEBUG)
			printf("push %ld %ld\n",bottom,top);
	}
	//**********************************************

	//***********Atomic compare and Swap top of the task deque************
	bool compareAndSwapTop(long oldV,long newV)
	{
		bool r;
		pthread_mutex_lock(&tq_lock);
		r= (oldV==top);
		if(r)
			top=newV;
		pthread_mutex_unlock(&tq_lock);
		return r;
	}
	bool compareAndSwapTopAndTrace(long oldV,long newV,Task &c,int victim,int myrank)
	{
		bool r;
		pthread_mutex_lock(&tq_lock);
		r= (oldV==top);
		if(r)
			top=newV;

		if(r)
		{
			if(c.step!=0)
				printf("I think some problem here %d\n",c.step);
//			printf("steal from %d to %d at level %d and old value %d\n",victim,myrank,c.level,wsh[victim].wpi.back().nTasksStolen[c.level]);
			if(!wsh[victim].wpi.back().validate()){
				printf("MOTHER FUCKER here it is\n");
				printf("*******%d %d %d %d %d\n",c.level,wsh[victim].wpi.size()-1,wsh[victim].wpi.back().nTasksStolen[c.level],myrank,victim);
			}
			
			int tempttt=wsh[victim].wpi.back().nTasksStolen[c.level];

			wsh[victim].wpi.back().nTasksStolenSet(c.level,  wsh[victim].wpi.back().nTasksStolen[c.level]+1 );
			wsh[victim].wpi.back().thieves.push_back(myrank);

			if(!wsh[victim].wpi.back().validate()){
				printf("MOTHER FUCKER here it is\n");
				printf("*******%d %d %d %d %d %d\n",c.level,wsh[victim].wpi.size()-1,tempttt,wsh[victim].wpi.back().nTasksStolen[c.level],myrank,victim);
			}


		}
		pthread_mutex_unlock(&tq_lock);

			HelpFirstInfo phase(&c);	//on new phase c.level become 0
			phase.victim = victim;
			phase.nTasksStolen.push_back(c.level); //c.level here is 0 always as a new phase will start and base level be 0
			wsh[myrank].addWorkingPhase(myrank,phase);

		return r;

	}
	//*******************************************************************

	//************Pop from top**********************
	Task *steal(int victim,int myrank)
	{
		long t = top;
		long b = bottom;
		if(b <= t) //TaskList is Empty
			return NULL;
		Task *task=tasks.get(t);
		if(!TRACE)
		{
			if(!compareAndSwapTop(t,t+1))
				return NULL;
		}
		else
			if( !compareAndSwapTopAndTrace(t,t+1,*task,victim,myrank) )
				return NULL;

		if(DEBUG)
			printf("steal %ld %ld\n",bottom,top);
		return task;

	}
	//*************************************************

	//**************Pop from Bottom*****************
	Task *pop()
	{
		long b= (--bottom);
		long t= top;
		if(b<t) //If Task List was already empty then
		{
			bottom = t;	
			return NULL;
		}
		Task *task=tasks.get(b);
		if(b>t)	//If Task List has more than one element then just return no changes to top
		{

#if DEBUG
				printf("pop %ld %ld\n",bottom,top);
#endif
			return task;
		}
		if(!compareAndSwapTop(t,t+1))
		{
			//printf("ERROR : Concurrent Steel \n");
			return NULL;
		}
		bottom=t+1;	//top is t+1 so bottom should also be t+1 for empty
#if DEBUG
			printf("pop %ld %ld\n",bottom,top);
#endif
		return task;
	}
	//************************************************


};


DeQueue *workerShelves;

void WorkingStateHdr::addWorkingPhase(int myrank,HelpFirstInfo &new_wpi)
{
	pthread_mutex_t *tq_lock = &(workerShelves[myrank].tq_lock);
	pthread_mutex_lock(tq_lock);
	(this->wpi).push_back(new_wpi);	
	pthread_mutex_unlock(tq_lock);
}	

Task *grab_task_from_runtime()
{
	int *p = (int *)pthread_getspecific(key);
	if(p==NULL)
	{
		printf("ERROR :key-value not set yet\n");
		return NULL;
	}
	Task *func=workerShelves[*p].pop();

	//No more task so try to steal or change the workingphase
	if(func==NULL)
	{
		if(!REPLAY)
		{
			int victim;
			do{
				victim = rand()%size;
			}while(victim== (*p));
			func = workerShelves[victim].steal(victim,*p);
			if(func!=NULL)
				printf("steal from %d to %d\n",victim,*p);
		}
		else
		{
			if(replay_wsh[*p].isMoreWPI())
			{
				replay_wsh[*p].current_wpi = replay_wsh[*p].getNextWPI();
				int victim = replay_wsh[*p].current_wpi.victim;
				// printf("current_wpi for %d\n",*p);
				// replay_wsh[*p].current_wpi.print();
				printf("current phase index : %d, waiting %d for %d\n",replay_wsh[*p].count_wpi-1,*p,victim);
				do{
					if((replay_wsh[*p].recPipe[victim])==NULL)
						printf("mother fucker %d and %d\n",*p,victim);
					func = (replay_wsh[*p].recPipe[victim])->popTop();
				}while(func==NULL);
				printf("current phase index : %d, waiting complete %d for %d\n",replay_wsh[*p].count_wpi-1,*p,victim);
				printf("DEBUG : WORKER:%d WPI:%d\n",*p,replay_wsh[*p].count_wpi-1);
				func->atFrontier = true; 	//mark that its child could get stolen
				func->level = 0;
				replay_wsh[*p].current_wpi.childCount.push_back(0);	//at level 0 there are currently 0 children found
				replay_wsh[*p].current_wpi.childCount.push_back(0);	//at level 1 there are currently 0 children found here 0th level task put childCount	
			}
			else
				func = NULL;
		}
	}
	return func;
}

void push_task_to_runtime(Task l){
	int *p = (int *)pthread_getspecific(key);
	if(p==NULL)
	{
		printf("ERROR :key-value not set yet\n");
		return ;
	}
	workerShelves[*p].push(l);
}
//****************************************************************
//****************************************************************


//**************************************************************************************************
//**************************************************************************************************


void find_and_execute_task()
{
	int *p = (int *)pthread_getspecific(key);
	Task *func=grab_task_from_runtime();
	if(TRACE || REPLAY)
		executingTasks[*p] = func;

	if(func!=NULL){
		if(REPLAY)
			replay_wsh[*p].current_wpi.childCount[func->level+1] = 0;
		(func->thisT)();
#ifdef fin_lock
		{
			pthread_mutex_lock(&fin_lock);
			finish_counter--;
			pthread_mutex_unlock(&fin_lock);
		}
#else
		__atomic_fetch_add(&finish_counter, (-1), __ATOMIC_SEQ_CST); //finish_counter--
#endif
	}
}


void *worker_routine(void *p){
	//**********For Work Steeling Deque**********
	pthread_setspecific(key,p);
#if DEBUG
		printf("created %d\n",*((int *)p) );
#endif
	//*******************************************
	while(!shutdown)
	{
		find_and_execute_task();
	}
	return NULL;
}

void create_workers(int &size)
{
	//**********For Work Steeling Deque**********
	workerShelves = new DeQueue[size];	
	pthread_key_create(&key,[&](void *){
		//delete workerShelves;
	});
	int *pt = (int *)malloc(sizeof(int));
	*pt = size-1;
	pthread_setspecific(key,pt);
	//*******************************************
	threads=(pthread_t *)malloc(sizeof(pthread_t)*(size-1));
	for(int i=0;i<size-1;i++){
		//*************For Work Steeling Deqeue********
		int *temp_i=(int *)malloc(sizeof(int));
		*temp_i = i;
		//**********************************************
		pthread_create(&threads[i],NULL, worker_routine, (void *)temp_i);
	}

}


// Just a helper funtion for char * to bool
bool string2bool(char *s)
{
	char b[8];
	if ( sscanf(s,"%[TtRrUuEe]",b))
	   return true;
	else
	   return false;
}


// ########This method is to get the environment variable decribed as below
// ########COTTON_WORKERS
// ########COTTON_TRACE
// ########COTTON_REPLAY
void getEnVariable()
{
	char *p = getenv("COTTON_WORKERS");
	if(p!=NULL)
		sscanf(p,"%d",&size);
	else
		size=1;
	p = getenv("COTTON_REPLAY");
	if(p!=NULL)
	{
		printf("Replaying the steel execution\n");
		REPLAY = string2bool(p);
	}
	p = getenv("COTTON_TRACE");
	if(p!=NULL)
	{
		printf("Tracing the steal execution\n");
		TRACE = string2bool(p);
	}
	p = getenv("PRINT");
	if(p!=NULL)
	{
		printf("Printing temp.trace : \n");
		replay_wsh = readReplayWorkerStateHdrs("temp.trace");
		for(int i=0;i<size;i++)
		{
			printf("WORKER %d list of workphase\n",i);
			replay_wsh[i].print();
		}
		exit(0);
	}

}

void init_runtime()
{
	shutdown=false;
	//thread_pool_size

	getEnVariable();

	if(TRACE)
		init_tracing();
	if(REPLAY)
		init_replay();

#if DEBUG
		printf("WORKERS : %d\n",size);
#endif
	srand(time(NULL));
	//****************
	create_workers(size);
}

void updateNewTask(Task &t)
{
	//***********async(Task t,Cont this) spawning task t when exectcuting task 'this'
	int *p = (int *)pthread_getspecific(key);	
	Task *th = executingTasks[*p];
	t.level=(th->level)+1;
	t.step=0;
	(th->step)+=1;

	//*********************************************************************************************************
}

void updateLevelsNum(Task &t)
{
	int *p = (int *)pthread_getspecific(key);	
	if(TRACE)
	{
		//If a new level is created because of this async then initialize it with 0
		while(t.level >= wsh[*p].wpi.back().nTasksStolen.size() )
			wsh[*p].wpi.back().nTasksStolenPush_Back(0);
	}
	else if(REPLAY)
	{
		while(t.level+1 >= replay_wsh[*p].current_wpi.childCount.size())
			replay_wsh[*p].current_wpi.childCount.push_back(0);
	}
}


// This will update childCount and send task which will be stolen to the theif worker
bool updateStealTask(Task &t)
{
	bool isTaskPush = true;
	int *p = (int *)pthread_getspecific(key);	
	if(executingTasks[*p]->atFrontier)
	{
	//	printf("here %d %d %d\n",t.level,replay_wsh[*p].current_wpi.childCount[t.level],replay_wsh[*p].current_wpi.nTasksStolen[t.level]);
		if(replay_wsh[*p].current_wpi.childCount[t.level] < getTempMyFuck(*p,t.level) )
		{
			isTaskPush = false;
			markStolen(*p,t,replay_wsh[*p].current_wpi);
		}
		else if(replay_wsh[*p].current_wpi.childCount[t.level] == getTempMyFuck(*p,t.level))
			t.atFrontier = true;
	}

	replay_wsh[*p].current_wpi.childCount[t.level]+=1;
	return isTaskPush;
}

void async(std::function<void()> &&lambda){
	Task t(lambda);
	bool isTaskPush = true;	//if this task is stolen in replay then not pushed inside runtime
	if(TRACE){
		updateNewTask(t);
		updateLevelsNum(t);
	}
	if(REPLAY)
	{
		updateNewTask(t);
		updateLevelsNum(t);
		isTaskPush = updateStealTask(t);
	}
	if(isTaskPush)
		push_task_to_runtime(t);
#if fin_lock
		{
			pthread_mutex_lock(&fin_lock);
			finish_counter++;
			pthread_mutex_unlock(&fin_lock);
		}
#else
		__atomic_fetch_add(&finish_counter, 1, __ATOMIC_SEQ_CST); //finish_counter++
#endif

}


void start_finish(){ 
	finish_counter=0;
}

void end_finish(){
	while(finish_counter!=0){
		find_and_execute_task();
	}
}

void finalize_runtime(){
	shutdown=true;
	for(int i=0;i<size-1;i++){
		pthread_join(threads[i],NULL);
	} 
	free(threads);

	if(TRACE)
	{
		printf("*************Writing Trace to local file named 'temp.obj'\n");
		writeWorkerStateHdrs("temp.trace",wsh);

		bool check=false;
		printf("****************Validating the tracing************* \n");
		for(int i=0;i<size;i++)
		{
			//printf("WORKER %d list of workphase\n",i);
			//wsh[i].print();
			if(!wsh[i].validate()){
				printf("INCORRECT VALIDATE\n");
				check=true;
				printf("ERROR: on :\n");
				printf("WORKER %d list of workphase\n",i);
				wsh[i].print();
			
			}
		}
		if(check)
		{
			printf("****************Retracing************* \n");
			
			for(int i=0;i<size;i++)
			{
				printf("WORKER %d list of workphase\n",i);
				wsh[i].print();
			}
		}	
	}



}


//***************************************************
//**************************************************
}

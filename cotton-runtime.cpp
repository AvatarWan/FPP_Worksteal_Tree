
/**************************************************************************************************************/
/* This is the cotton runtime a very lightweight implementation of the library-based concurrency platform that
/* should support basic async-finish based task-parallelism and rely on “work-stealing” technique for task scheduling.
/* This runtime is only able to support “flat finish” scopes. A flat-finish scope is a single-level finish scope
/* for all async tasks spawned within its scope (i.e., an async task can recursively create new async tasks but
/* it can never spawn new finish scopes).
/*
/* This code later is modified and included the Low-Overhead Tracing of Work Stealing Schedulers and Replay 
/* of the program with the same steal tree as explained the paper :
/* *************************"Steal Tree: Low-Overhead Tracing of Work Stealing Schedulers"*********************/
/*                              by Jonathan Lifflander, Sriram Krishnamoorthy and
/*                                           Laxmikant V. Kale
/**************************************************************************************************************/

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

//Scope for the all linguistic interfaces that are implemented
namespace cotton
{

/*******************************************************************************************************/
/*
/*******************************************************************************************************/
//This flag could be used to help debug the print the Working info of the Code
#define DEBUG false
//This is used for updation of the finish_counter
pthread_mutex_t fin_lock=PTHREAD_MUTEX_INITIALIZER; //finish_counter lock if defined then use mutex_lock

//Variable that keeps the state of the whether runtime is running or not
volatile bool shutdown=true; //true-not running,false-running

//Number of the Workers
int size=1;
pthread_t *threads;
//Finish counter to store number of Task under deque or getting executed
volatile int finish_counter=0;

//Used to assign id to each worker
pthread_key_t key;
//Used for debuging the client code
int getKey()
{
	return 	*((int *)pthread_getspecific(key));
}
int getSize()
{
	return size;
}


/*********************************************************************************************************/
/* Code used for Tracing and Replay fo the Workstealing Scheduler that uses Help First Approch
/* The Psedo code and explaination of the Working could be found over the mentioned Paper by Jonathan Lifflander
/*********************************************************************************************************/

/*********************************************************************************************************/
/* REPLAY - flag for replay of the steal execution
/* TRACE  - flag to signify Tracing of the steal operations
/* FILE_LOC - will be used to save and read the tracing info for both replaying and tracing
/*********************************************************************************************************/
bool REPLAY = false;
bool TRACE = false;
char *FILE_LOC="temp.trace";


/********************************************************************************************************/
/* Data structure for the partially completed task or running task or untouched (full) task
/* (Note : because this is a flat finish runtime so a task if stared execution then will definetly complete
/* or the whole task will get stolen i.e. no partial task could ever get stolen) so "step" data member is
/* not used anywhere is the Tracing or Replaying algorithem
/* thisT :- task that needs to be executed (passed on the async interface of the runtime)
/* level :- level the working tree that the task is asynced (encontered in the client code)
/* step  :- number of steps that are executed of the task (dynamic sequence of the instruction that has no
/*			interleaving async, finish)
/********************************************************************************************************/
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
/***************************************************************************************************************/
/* To signify a any kind of Task
/* In replay a rather than creating a new data member i have used the same class with atFrontier
/* atFrontier :- During replay each task tracks whether its children could have been stolen in the trace
/***************************************************************************************************************/
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

//Not used class as there is no continuation that could be stolen
class Cont : public  ContinuationHdr
{

};

/*********************************************************************************************************************/
/* Sending and Reciving the Task between Workers Used for the Replay execution
/* as during the replay execution some task are need to marked and send to the theif for future execution so this 
/* data structure is used which will make a directed pipeline between two workers
/* push   :- push from the victim end of the task
/* popTop :- pop from the theif end of the task
/*********************************************************************************************************************/
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


// 	***********Push to Bottom********************
// [PROBLEM]# Here we need to work as push and popTop shouldn't require a lock but without it not working
	void push(Task &task)
	{

		pthread_mutex_lock(&tq_lock);
		long b=bottom;
		long t=top;
		if((b-t) >= (tasks.size()-1) )	//TaskList is Full so grow it
		{
			#if DEBUG 
				printf("Regrowing Task List for %d\n",0);
			#endif
			tasks.grow(b,t);
		}
		tasks.put(b,task);
		bottom = b+1;
		
		pthread_mutex_unlock(&tq_lock);
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


/****************************************************************************************************************/
/* Stores the information of the possible executing countinuation steps in well defined order that start start with single continuation 
/* after every steal working phase changes so it needs to update itself on each steal from it
/* on steal by theif a new working phase will start for the theif so needs to store the victim task has been ston
/* from
/* victim :- worker from which the task stolen which start this working phase
/* stepStolen :- not used as explained in the ContinuationHdr step is not used because of the flat finish and help first
/* thieves :- list the thieves that stole the task from this working phase
/* getNextTheif() :- return the theif ids in the order at which they steal tasks (only used in replay)
/* isMoreTheif() :- is more theif available that could steal the task from working phase (only used in replay)
/****************************************************************************************************************/
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
		#if DEBUG
			printf("ERROR: no more theif\n");
		#endif
		return -1;
	}
	bool isMoreTheif()
	{
		return curr_theif < thieves.size();
	}
protected:
	WorkingPhaseInfo(){}
};


/************************************************************************************************************/
/* Used for the Help First working scheduler Working Phase
/* nTaskStolen :- number of task stolen at each level, initialize to 0 when encountered a new level
/* nTasksStolen_lock :- because nTasksStolen could be modified when a new level is created by victim or theif stealing and modifying the
/* task count so have to use a Lock
/* childCount :- number of children for current executing task (used in replay Only)
/************************************************************************************************************/
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

/********************************************************************************************************/
/*These funtions are created to put the trace of the running code to a local file. 						*/
/********************************************************************************************************/


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



/*********************************************************************************************************/
/* For each worker there will be different working Phases (after every steal it will change its working 
/* Phase) so this structure stores the list of all the working phases
/* wpi    :- list of the working phases of the worker
/* validate :- helper function to validate the theif and nTasksStolen list of each working phase
/*********************************************************************************************************/
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

	pthread_mutex_t lk_wpi=PTHREAD_MUTEX_INITIALIZER;

	void addNewPhase(HelpFirstInfo &phase)
	{
		pthread_mutex_lock(&lk_wpi);
		wpi.push_back(phase);
		pthread_mutex_unlock(&lk_wpi);
	}

	void addTheifToPhase(int index,int level,int thief)
	{
		pthread_mutex_lock(&lk_wpi);

		HelpFirstInfo *curr_wpi=&(wpi[index]);
		curr_wpi->nTasksStolenSet(level,  (curr_wpi->nTasksStolen)[level]+1);
		curr_wpi->thieves.push_back(thief);

		pthread_mutex_unlock(&lk_wpi);		
	}

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


/************************************************************************************************************/
/* Replay Working state Hdr to store working phase list of one worker and pipelines to tansfer and get Task
/* from others
/* current_wpi :- working phase that worker currently executing
/* sendPipe :- pipeline to send Task that will be stolen by other from this worker
/* recvPipe :- pipeline to receive Task that will be stolen by this worker from other and this will could
/* 			   change in working state phase
/************************************************************************************************************/
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
					//Send stolen task from i to j
					PipeDeQueue *temp = new PipeDeQueue();
					replay_wsh[i].sendPipe[j] = temp;
					replay_wsh[j].recPipe[i] = temp;
				}
				else{
					replay_wsh[i].sendPipe[j]=NULL;
					replay_wsh[j].recPipe[i]=NULL;
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
	if(size < tempSize)
	{
		printf("[ERROR]: Replaying require more number of Workers\n");
		exit(1);
	}
	else if(size > tempSize)
	{
		printf("[ERROR]: Tracing has less number of Workers=%d\n",tempSize);
		size = tempSize;
		exit(1);
	}

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

/***********************************************************************************************************/
/* Initialize the first executing task and space allocate the area to store reference to currently executing
/* Task by the Workers as this information is usefull to check whether current task is frontliner or not
/* for updateStealCount which update childCount for currently executing Task Working phase and fronliner 
/* info for child Tasks
/***********************************************************************************************************/
void init_executionTask()
{
	//This is keeping track of which worker executing which task currently
	executingTasks = (Task **) malloc(sizeof(Task *)*size);
	//*****Remember initialy size-1 rank worker with execute the root task i.e. start_finish task
	executingTasks[size-1] = Task::dummyTask(); // root task that work at start_finish

}

int getVictimeCurrentStolen(int victim,int level)
{
	if(level>=replay_wsh[victim].current_wpi.nTasksStolen.size())
		return 0;
	else
		return replay_wsh[victim].current_wpi.nTasksStolen[level];

}

/*****************************************************************************************************/
/* If during replay async, worker find that any task will be stolen by any other worker then it be mark it as
/* stolen and send toward theif worker using pipelines and don't push into its deque
/*****************************************************************************************************/
void markStolen(int victim,Task &t,HelpFirstInfo &current_wpi)
{
	int thief;
	//enqueue't' for sending to the next theif in current working phase info's thieves
	if(current_wpi.isMoreTheif())
		thief = current_wpi.getNextTheif();
	else{
		printf("[ERROR] : no more theif,for worker:%d ,count_wpi:%d ,last value of curr_theif:%d \n",victim,replay_wsh[victim].count_wpi-1,current_wpi.curr_theif);
		printf("[ERROR] : level:%d nChild:%d nTasksStolen:%d\n",t.level,current_wpi.childCount[t.level],getVictimeCurrentStolen(victim,t.level) );
	}
	(replay_wsh[victim].sendPipe[thief])->push(t);
	#if DEBUG
		printf("[DEBUG] : %d can take from %d\n",thief,victim);
	#endif
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

	pthread_mutex_t tq_lock=PTHREAD_MUTEX_INITIALIZER; //task queue lock
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
		// if(DEBUG)
		// 	printf("push %ld %ld\n",bottom,top);
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
		int vict_phase_index = wsh[victim].wpi.size()-1;
		r= (oldV==top);
		if(r)
		{
			top=newV;

			//Unused and unnessesary code
			// if(c.step!=0)
			// 	printf("I think some problem here %d\n",c.step);


//			printf("steal from %d to %d at level %d and old value %d\n",victim,myrank,c.level,wsh[victim].wpi.back().nTasksStolen[c.level]);
			// if(!wsh[victim].wpi[vict_phase_index].validate()){
			// 	printf("Victim phase info is incorrect\n");
			// 	printf("*******%d %d %d %d %d\n",c.level,vict_phase_index,wsh[victim].wpi[vict_phase_index].nTasksStolen[c.level],myrank,victim);
			// }
			
			int tempttt=wsh[victim].wpi[vict_phase_index].nTasksStolen[c.level];

			wsh[victim].addTheifToPhase(vict_phase_index,c.level,myrank);

			#if DEBUG
				if(!wsh[victim].wpi[vict_phase_index].validate()){
					printf("Victim phase info is incorrect\n");
					printf("*******%d %d %d %d %d %d\n",c.level,vict_phase_index,tempttt,wsh[victim].wpi[vict_phase_index].nTasksStolen[c.level],myrank,victim);
				}
			#endif

			HelpFirstInfo phase(&c);	//on new phase c.level become 0
			phase.victim = victim;
			phase.nTasksStolen.push_back(c.level); //c.level here is 0 always as a new phase will start and base level be 0
			wsh[myrank].addNewPhase(phase);
		}
		pthread_mutex_unlock(&tq_lock);
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
		else //in case of Trace if steal is successfull then trace it too
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
				// printf("pop %ld %ld\n",bottom,top);
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
			// printf("pop %ld %ld\n",bottom,top);
#endif
		return task;
	}
	//************************************************


};


DeQueue *workerShelves;
	

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
		//In case of Normal or Traced Execution
		if(!REPLAY)
		{
			int victim;
			do{
				victim = rand()%size;
			}while(victim== (*p));
			func = workerShelves[victim].steal(victim,*p);
			#if DEBUG
			{
				if(func!=NULL)
					printf("steal from %d to %d\n",victim,*p);
			}
			#endif
		} //In case of Replay rather then stealing use the task from pipelines of the predetermined victim
		else
		{
			/*******************************************************************************************************************/	
			/* Check if there is more Working Phase for the Worker if there is no more available task in deque(Working Shelf) and
			/* no more task for steal so finish the execution and suspend this Worker 
			/*******************************************************************************************************************/
			if(replay_wsh[*p].isMoreWPI())
			{
				replay_wsh[*p].current_wpi = replay_wsh[*p].getNextWPI();
				int victim = replay_wsh[*p].current_wpi.victim;
				#if DEBUG
					printf("DEBUG : %d, waiting %d for %d\n",replay_wsh[*p].count_wpi-1,*p,victim);
				#endif
				do{
					func = (replay_wsh[*p].recPipe[victim])->popTop();
				}while(func==NULL && finish_counter!=0);
				if(finish_counter==0)
					return NULL;
				#if DEBUG
				{
					printf("DEBUG : %d, waiting complete %d for %d\n",replay_wsh[*p].count_wpi-1,*p,victim);
					printf("DEBUG :Change in Working Phase - WORKER:%d WPI:%d\n",*p,replay_wsh[*p].count_wpi);
				}
				#endif
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
	if(TRACE || REPLAY)			//Update the currenlty executing task by the worker
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
	if (sscanf(s,"%[TtRrUuEe]",b) || s[0]=='1')
	   return true;
	else
	   return false;
}


// # false for error
bool checkEnVariables()
{
	if(TRACE && REPLAY){
		printf("ERROR : Replay and Trace aren't supported at same execution\n");
		return false;
	}
	return true;
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
		REPLAY = string2bool(p);
		if(REPLAY)
			printf("Replaying the steel execution\n");
	}
	p = getenv("COTTON_TRACE");
	if(p!=NULL)
	{
		TRACE = string2bool(p);
		if(TRACE)
			printf("Tracing the steal execution\n");
	}
	p = getenv("COTTON_FILE");
	if(p!=NULL)
	{
		FILE_LOC=p;
	}
	p = getenv("PRINT");
	if(p!=NULL)
	{
		printf("Printing temp.trace : \n");
		replay_wsh = readReplayWorkerStateHdrs(FILE_LOC);
		for(int i=0;i<size;i++)
		{
			printf("WORKER %d list of workphase\n",i);
			replay_wsh[i].print();
		}
		exit(0);
	}
	if(!checkEnVariables())
		exit(1);
}


void init_replay()
{
	init_executionTask();
	executingTasks[size-1]->atFrontier = true;
	replay_wsh = readReplayWorkerStateHdrs(FILE_LOC);
	ReplayWorkingStateHdr::setPipeLines(replay_wsh);
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
void init_runtime()
{
	shutdown=false;
	getEnVariable();

	if(TRACE)
		init_tracing();
	if(REPLAY)
		init_replay();

#if DEBUG
		// printf("WORKERS : %d\n",size);
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
// In order to make space or initialize a new level in nTasksStolen and childCount
void updateLevelsNum(Task &t)
{
	int *p = (int *)pthread_getspecific(key);	
	if(TRACE)
	{
		//If a new level is created because of this async then initialize it with 0
		while(t.level+3 >= wsh[*p].wpi.back().nTasksStolen.size() )
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
		if(replay_wsh[*p].current_wpi.childCount[t.level] < getVictimeCurrentStolen(*p,t.level) )
		{
			isTaskPush = false;
			markStolen(*p,t,replay_wsh[*p].current_wpi);
		}
		else if(replay_wsh[*p].current_wpi.childCount[t.level] == getVictimeCurrentStolen(*p,t.level))
			t.atFrontier = true;
	}

	replay_wsh[*p].current_wpi.childCount[t.level]+=1;
	return isTaskPush;
}

void async(std::function<void()> &&lambda){
	// From function create a task (as task could have its own level and steps that are executed in it)
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
		printf("*************Writing Trace to local file named '%s'\n",FILE_LOC);
		writeWorkerStateHdrs(FILE_LOC,wsh);

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
		// if(check)
		// {
		// 	printf("****************Retracing************* \n");
			
		// 	for(int i=0;i<size;i++)
		// 	{
		// 		printf("WORKER %d list of workphase\n",i);
		// 		wsh[i].print();
		// 	}
		// }	
	}



}


//***************************************************
//**************************************************
}

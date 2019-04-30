#include "cotton.h"
#include <sys/time.h>
#include <stdlib.h>
#include <sys/time.h>

#define FDEBUG false

long get_usecs (void)
{
   struct timeval t;
   gettimeofday(&t,NULL);
   return t.tv_sec*1000000+t.tv_usec;
}

/*
int main(int argc, char* argv[])
{
  cotton::init_runtime();

  cotton::start_finish();

	cotton::async([=]() {
		printf("l11 executed %d\n",cotton::getKey());


		cotton::async([=]() {
			printf("l111 executed %d\n",cotton::getKey());
		});
		cotton::async([=]() {
			printf("l112 executed %d\n",cotton::getKey());
		});
		cotton::async([=]() {
			printf("l113 executed %d\n",cotton::getKey());
		});
	});

	cotton::async([=]() {
		printf("l12 executed by %d\n",cotton::getKey());
		cotton::async([=]() {
			printf("l121 executed %d\n",cotton::getKey());
		});
		cotton::async([=]() {
			printf("l122 executed %d\n",cotton::getKey());
		});
		cotton::async([=]() {
			printf("l123 executed %d\n",cotton::getKey());
		});
	});

	cotton::async([=]() {
		printf("l13 executed by %d\n",cotton::getKey());

	});

	cotton::async([=]() {
		printf("l14 executed by %d\n",cotton::getKey());

	});

	cotton::async([=]() {
		printf("l15 executed by %d\n",cotton::getKey());

	});
  cotton::end_finish();

  cotton::finalize_runtime();
  return 0;
}
*/

/*
	cotton::async([=]() {


	});


*/

int g=0;

volatile int *arr;

void fibAdd(int n)
{
	if(n<=1){
		__atomic_fetch_add(&g, n, __ATOMIC_SEQ_CST);
		#if FDEBUG
		{
			if(n==1)
			{
				arr[cotton::getKey()]++;
			}
		}
		#endif
		return ;	
	}
	cotton::async([=]() {
		fibAdd(n-1);
	});
	cotton::async([=]() {
		fibAdd(n-2);
	});

}

void verify(int n,int r)
{
	int sol[]={0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597, 2584, 4181, 6765, 10946, 17711, 28657, 46368, 75025, 121393, 196418, 317811, 514229, 832040, 1346269, 2178309, 3524578, 5702887, 9227465, 14930352, 24157817, 39088169, 63245986, 102334155, 165580141, 267914296, 433494437, 701408733, 1134903170, 1836311903};
	if(sol[n]==r)
		printf("RESULT: OK %d\n",sol[n]);
	else{
		printf("RESULT: INCORRECT %d!=%d\n",sol[n],r);
		printf("DEBUG : worker executed addition : \n");
	}
	#if FDEBUG
	{
		for(int i=0;i<cotton::getSize();i++)
			printf("%d ",arr[i]);
		printf("\n");
	}
	#endif
}

int main(int argc, char* argv[])
{

	int n=15;

  cotton::init_runtime();

  if(argc > 1) n = atoi(argv[1]);
  arr=(int *)calloc(cotton::getSize(),sizeof(int));

  double dur = 0;
  // Timing for parallel run
  long start = get_usecs();	
  cotton::start_finish();

  fibAdd(n);

  cotton::end_finish();
  // Timing for parallel run
  long end = get_usecs();
  dur = ((double)(end-start))/1000000;

  cotton::finalize_runtime();
  verify(n,g);
  printf("Fib(%d) Time = %fsec\n",n,dur);


  return 0;
}

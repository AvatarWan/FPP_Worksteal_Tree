#include "cotton.h"
#include <sys/time.h>
#include <stdlib.h>

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
		if(n==1)
		{
//			printf("fuck %d %d\n",cotton::getKey(),++(arr[cotton::getKey()]));
			arr[cotton::getKey()]++;
		}
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
	int sol[]={0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597, 2584, 4181, 6765, 10946, 17711, 28657, 46368, 75025, 121393, 196418, 317811};
	if(sol[n]==r)
		printf("RESULT: OK %d\n",sol[n]);
	else{
		printf("RESULT: INCORRECT %d!=%d\n",sol[n],r);
		printf("DEBUG : worker executed addition : \n");
	}
	for(int i=0;i<cotton::getSize();i++)
		printf("%d ",arr[i]);
	printf("\n");
	
}

int main(int argc, char* argv[])
{

	int n=5;

  cotton::init_runtime();

  if(argc > 1) n = atoi(argv[1]);
  printf("n:%d\n",n);
  arr=(int *)calloc(cotton::getSize(),sizeof(int));
for(int i=0;i<cotton::getSize();i++)
			printf("%d ",arr[i]);
		printf("\n");
	
  cotton::start_finish();

  fibAdd(n);

  cotton::end_finish();

  cotton::finalize_runtime();
  verify(n,g);


  return 0;
}

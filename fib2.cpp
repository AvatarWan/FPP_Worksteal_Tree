#include <stdlib.h>
#include <stdio.h>

int arr[50];

int fib(int n)
{
	if(arr[n]== -1)
	{
		if(n<=1)
			arr[n]=n;
		else
		{
			int a=fib(n-1);
			int b=fib(n-2);
			arr[n]=a+b;
		}
	}
	return arr[n];
}

int main(int argc, char const *argv[])
{
	for(int i=0;i<50;i++)
		arr[i]= -1;

	printf("%d\n",fib(49));
	for(int i=0;i<50;i++)
		printf("%d, ",arr[i]);
	printf("\n");
	return 0;
}
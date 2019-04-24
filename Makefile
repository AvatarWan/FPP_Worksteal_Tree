EXE = nqueens temp

all: $(EXE)

cotton-runtime:cotton-runtime.cpp
	g++ -g -std=c++11 -O0 -pthread $<

cotton-runtime.o:cotton-runtime.cpp
	g++ -g -std=c++11 -pthread -c -O0 -o $@ $<

temp: cotton.h cotton-runtime.h cotton-runtime.o temp.cpp
	g++ -g -std=c++11 cotton.h cotton-runtime.h cotton-runtime.o temp.cpp -O0 -o temp -pthread

nqueens: cotton.h cotton-runtime.h cotton-runtime.o nqueens.cpp
	g++ -g -std=c++11 cotton.h cotton-runtime.h cotton-runtime.o nqueens.cpp -O0 -o nqueens -pthread

clean: 
	rm $(EXE) 
	 

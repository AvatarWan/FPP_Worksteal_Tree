How to run the code :

1 First compile the whole code using
	$make

2.1 Normal Execution :
	$ COTTON_WORKER=4 ./nqueens

2.2 Tracing :
		Using default File to write trace info
	$ COTTON_WORKER=4 COTTON_TRACE=1 ./nqueens
			or
		Providing File name to write trace info
	$ COTTON_WORKER=4 COTTON_TRACE=1 COTTON_FILE=nqueens.trace ./nqueens

2.3 Replay :
		Using default File to read trace info
	$ COTTON_WORKER=4 COTTON_REPLAY=1 ./nqueens
			or
		Providing File name to read trace info
	$ COTTON_WORKER=4 COTTON_REPLAY=1 COTTON_FILE=nqueens.trace ./nqueens

2.4 Print Tracing Info from File :
	$ COTTON_WORKER=4 PRINT=1 ./nqueens
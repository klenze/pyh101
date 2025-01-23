all: testme
	pip3 install .


%.o: %.cxx
	g++ -Wall -g -c -I${UCESB_DIR}/hbook -o $@ $<


%.o: %.c
	gcc -Wall -g -c -I${UCESB_DIR}/hbook -o $@ $<

testme: test.o ext_data_client.o
	g++ -Wall -g $^  -o $@

clean: 
	rm -f testme *.o


	#g++ -Wall -g $^ -L${UCESB_DIR}/hbook -l ext_data_clnt -o $@
test: testme
	/u/land/fake_cvmfs/11/extra_jan24p1/upexps/202503_s122/202503_s122 /d/land6/202503_s122/lmd/main0003_0001.lmd --allow-errors --input-buffer=138Mi --max-events=2 --ntuple=STRUCT,RAW,- | ./testme


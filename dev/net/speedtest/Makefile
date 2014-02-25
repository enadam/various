#!/usr/bin/make -f

.PHONY: test clean

speedtest: speedtest.c
	cc -Wall -O2 -s -lsctp -lrt $< -o $@;

test: speedtest
	./regtest

clean:
	rm -f speedtest regtest-client.log regtest-server.log;

# End of Makefile

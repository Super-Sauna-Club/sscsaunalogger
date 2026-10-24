CFLAGS = -std=c11 -O1 -Wall -Wextra -Werror -g
SRC = ../SenseCAP_Indicator_RP2040/src/ssc

all: test_detect test_proto
	./test_detect
	./test_proto

test_detect: test_detect.c $(SRC)/ssc_detect.c $(SRC)/ssc_detect.h
	$(CC) $(CFLAGS) -o $@ test_detect.c $(SRC)/ssc_detect.c -lm

test_proto: test_proto.c $(SRC)/ssc_proto.h
	$(CC) $(CFLAGS) -o $@ test_proto.c

clean:
	rm -f test_detect test_proto

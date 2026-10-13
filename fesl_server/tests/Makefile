# FESL decoder tests, replaying the decrypted captures in stuff/pcap
#   make test               build and run
#   make test SANITIZE=1    same with AddressSanitizer/UBSan

CFLAGS		+= -O2 -g -Wall -Wextra -I../src/server
ifdef SANITIZE
CFLAGS		+= -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS		+= -fsanitize=address,undefined
endif

DATA		= data
CAPTURES	= $(DATA)/d1/dump2_ssl.pcap $(DATA)/d2/dump2_ssl.pcap

SERVER		= ../src/server
SESSION_SRC	= $(SERVER)/fesl_session.c $(SERVER)/accounts.c $(SERVER)/fesl.c

all: test_fesl test_fesl_session

test_fesl: test_fesl.c pcap_stream.c pcap_stream.h $(SERVER)/fesl.c $(SERVER)/fesl.h
	$(CC) $(CFLAGS) test_fesl.c pcap_stream.c $(SERVER)/fesl.c -o $@ $(LDFLAGS)

test_fesl_session: test_fesl_session.c pcap_stream.c pcap_stream.h $(SESSION_SRC) $(SERVER)/*.h
	$(CC) $(CFLAGS) test_fesl_session.c pcap_stream.c $(SESSION_SRC) -o $@ $(LDFLAGS)

$(DATA)/%/dump2_ssl.pcap: ../stuff/pcap/%.zip
	mkdir -p $(DATA)/$*
	unzip -o -q $< -d $(DATA)/$*
	touch $@

test: test_fesl test_fesl_session $(CAPTURES)
	./test_fesl $(DATA)
	./test_fesl_session $(DATA)

clean:
	rm -rf test_fesl test_fesl_session $(DATA)

.PHONY: all test clean

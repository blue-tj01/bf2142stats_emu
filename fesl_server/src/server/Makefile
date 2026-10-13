# FESL server
#   make          build against the system OpenSSL (no SSLv3, use -X tls)
#   make ssl3     build against the private SSLv3 OpenSSL, needed by the game

EXE		= fesl_server
CFLAGS		+= -O2 -g -Wall -Wextra
SRC		= fesl_server.c fesl_session.c fesl.c accounts.c
HDR		= fesl.h fesl_session.h accounts.h

SSL3_DIR	= ../openssl-ssl3

all: $(EXE)

$(EXE): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $@ -lssl -lcrypto -lpthread

ssl3:
	../build-openssl-ssl3.sh
	$(CC) $(CFLAGS) -I$(SSL3_DIR)/include $(SRC) -o $(EXE) \
		$(SSL3_DIR)/lib/libssl.a $(SSL3_DIR)/lib/libcrypto.a -lpthread -ldl

clean:
	rm -f $(EXE)

.PHONY: all ssl3 clean

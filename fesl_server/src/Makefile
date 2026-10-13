EXE		= stcppipe
CFLAGS		+= -O2 -s

DESTDIR		=
prefix		= /usr/local
exec_prefix	= $(prefix)
bindir		= $(exec_prefix)/bin
BINDIR		= $(DESTDIR)$(bindir)

SRC		= $(EXE).c
LIBS		= -lssl -lcrypto -lpthread

# private OpenSSL with SSLv3 enabled, see build-openssl-ssl3.sh
SSL3_DIR	= openssl-ssl3

all:
	$(CC) $(SRC) $(CFLAGS) -o $(EXE) $(LIBS)

# stcppipe statically linked against OpenSSL with SSLv3 support
ssl3:
	./build-openssl-ssl3.sh
	$(CC) $(SRC) $(CFLAGS) -I$(SSL3_DIR)/include -o $(EXE) \
		$(SSL3_DIR)/lib/libssl.a $(SSL3_DIR)/lib/libcrypto.a -lpthread -ldl

install:
	install -m 755 -d $(BINDIR)
	install -m 755 $(EXE) $(BINDIR)

clean:
	rm -f *.o $(EXE)

distclean: clean
	rm -rf $(SSL3_DIR)

.PHONY: all ssl3 clean distclean install

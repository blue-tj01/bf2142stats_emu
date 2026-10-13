# FESL server
#   make          build against the system OpenSSL (no SSLv3, use -X tls)
#   make ssl3     build against the private SSLv3 OpenSSL, needed by the game
#   MYSQL=0       build without database support even if the MySQL or
#                 MariaDB client library is installed

EXE		= fesl_server
CFLAGS		+= -O2 -g -Wall -Wextra
SRC		= fesl_server.c fesl_session.c fesl.c accounts.c accounts_db.c
HDR		= fesl.h fesl_session.h accounts.h accounts_db.h

SSL3_DIR	= ../openssl-ssl3

# database support when mariadb_config or mysql_config is found
ifneq ($(MYSQL),0)
MYSQL_CONFIG	?= $(shell command -v mariadb_config || command -v mysql_config)
endif
ifneq ($(MYSQL_CONFIG),)
DB_CFLAGS	= -DFESL_WITH_MYSQL $(shell $(MYSQL_CONFIG) --cflags)
DB_LIBS		= $(shell $(MYSQL_CONFIG) --libs)
endif

all: $(EXE)

$(EXE): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(DB_CFLAGS) $(SRC) -o $@ -lssl -lcrypto $(DB_LIBS) -lpthread

# --exclude-libs keeps the static OpenSSL 1.1.1 symbols private, so the
# MySQL client library still uses the system OpenSSL it was built with
ssl3:
	../build-openssl-ssl3.sh
	$(CC) $(CFLAGS) -I$(SSL3_DIR)/include $(DB_CFLAGS) $(SRC) -o $(EXE) \
		$(SSL3_DIR)/lib/libssl.a $(SSL3_DIR)/lib/libcrypto.a -Wl,--exclude-libs,ALL \
		$(DB_LIBS) -lpthread -ldl

clean:
	rm -f $(EXE)

.PHONY: all ssl3 clean

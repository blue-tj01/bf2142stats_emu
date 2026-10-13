#!/usr/bin/env python3
# FESL server end-to-end test: starts fesl_server with -X tls (Python's ssl
# module can't speak SSLv3) and with -X none (plain TCP), and logs in like
# the game does.
#
# With FESL_DB_NAME set it also runs against the account table of a stats
# database (database/bf2142.sql), like tests/smoke_test.py does:
#   FESL_DB_HOST, FESL_DB_PORT, FESL_DB_NAME, FESL_DB_USER, FESL_DB_PASS
#             what the server connects with
#   DB_CMD    command that runs SQL read from stdin against that database,
#             used to add and remove the test accounts
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

import os
import shlex
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

SERVER = sys.argv[1] if len(sys.argv) > 1 else "../src/server/fesl_server"
PING_INTERVAL = 1

# the client Hello and Login as captured in stuff/pcap
HELLO = ("TXN=Hello\nclientString=bf2142-pc\nsku=125170\nlocale=ru\nclientPlatform=PC\n"
         "clientVersion=1.10.112.0\nSDKVersion=2.8.0.1.0\nprotocolVersion=2.0\nfragmentSize=2048\n")
LOGIN = ("TXN=Login\nreturnEncryptedInfo=1\nname=Tema567\npassword={password}\n"
         "machineId=\nmacAddr=$1c6f65d226e4\n")

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def encode(command, word, body):
    data = body.encode() + b"\0"
    return command.encode() + struct.pack(">II", word, 12 + len(data)) + data


class Client:
    def __init__(self, port, use_ssl):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        if use_ssl:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            ctx.set_ciphers("ALL:@SECLEVEL=0")
            self.sock = ctx.wrap_socket(self.sock)
        self.buf = b""

    def send(self, command, word, body):
        self.sock.sendall(encode(command, word, body))

    def recv(self):
        while True:
            if len(self.buf) >= 12:
                size = struct.unpack(">I", self.buf[8:12])[0]
                if len(self.buf) >= size:
                    pkt, self.buf = self.buf[:size], self.buf[size:]
                    word = struct.unpack(">I", pkt[4:8])[0]
                    pairs = {}
                    for line in pkt[12:].rstrip(b"\0").decode().split("\n"):
                        if line:
                            k, _, v = line.partition("=")
                            pairs[k] = v
                    return pkt[:4].decode(), word, pairs
            data = self.sock.recv(65536)
            if not data:
                return None
            self.buf += data

    def close(self):
        self.sock.close()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def login(port, use_ssl, name, password):
    c = Client(port, use_ssl)
    c.send("fsys", 0x80000001, HELLO)
    c.recv()
    c.recv()
    c.send("acct", 0x80000002, LOGIN.replace("name=Tema567", "name=" + name).format(password=password))
    answer = c.recv()
    c.close()
    return answer[2] if answer else {}


def run(method, cert, source, log, user_id="10", env=None, extra=None):
    use_ssl = method != "none"
    port = free_port()
    args = [SERVER, "-X", method, "-p", str(port), "-i", str(PING_INTERVAL)] + source
    if use_ssl:
        args += ["-c", cert]
    with open(log, "w") as logf:
        server = subprocess.Popen(args, stdout=logf, stderr=subprocess.STDOUT, env=env)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)

        # Hello, then the server MemCheck with number 0, and our answer to it
        c = Client(port, use_ssl)
        c.send("fsys", 0x80000001, HELLO)
        cmd, word, hello = c.recv()
        check((cmd, word, hello.get("TXN")) == ("fsys", 0x80000001, "Hello"), "Hello answer header")
        check(hello.get("domainPartition.subDomain") == "battlefield2142-2006", "Hello subDomain")
        check("%3a" in hello.get("curTime", ""), "Hello curTime format")
        cmd, word, memcheck = c.recv()
        check((cmd, word, memcheck.get("TXN")) == ("fsys", 0x80000000, "MemCheck"), "MemCheck from server")
        c.send("fsys", 0x80000000, "TXN=MemCheck\nresult=\n")

        # successful Login
        c.send("acct", 0x80000002, LOGIN.format(password="123456"))
        cmd, word, login = c.recv()
        check((cmd, word) == ("acct", 0x80000002), "Login answer header")
        check(login.get("displayName") == "Tema567" and login.get("userId") == user_id
              and login.get("profileId") == user_id, "Login account: %r" % login)
        check(len(login.get("lkey", "")) == 28 and "errorCode" not in login, "Login lkey")

        c.send("dobj", 0x80000003, "TXN=GetObjectInventory\ndomainId=eagames\nsubdomainId=bf2142\n"
               "partitionKey=online_content\nobjectIds.[]=1\nobjectIds.0=bf2142_bp1\n")
        cmd, word, inv = c.recv()
        check((cmd, word, inv) == ("dobj", 0x80000003, {"TXN": "GetObjectInventory", "entitlements.[]": "0"}),
              "GetObjectInventory answer")

        # the server Ping comes every PING_INTERVAL seconds, also while the
        # client keeps sending (our Ping answers get no reply)
        start = time.time()
        pings = []
        while time.time() - start < PING_INTERVAL * 2.6:
            c.send("fsys", 0x80000000, "TXN=Ping\n")
            c.sock.settimeout(0.2)
            try:
                pkt = c.recv()
            except (socket.timeout, ssl.SSLError, TimeoutError):
                pkt = None
            c.sock.settimeout(10)
            if pkt:
                check(pkt[:2] == ("fsys", 0) and pkt[2].get("TXN") == "Ping", "only Pings while busy")
                pings.append(time.time() - start)
        gaps = [b - a for a, b in zip(pings, pings[1:])]
        check(len(pings) >= 2 and all(g >= PING_INTERVAL * 0.7 for g in gaps),
              "Pings every %ds while busy: %r" % (PING_INTERVAL, pings))

        # not implemented yet
        c.send("subs", 0x80000004, "TXN=GetEntitlementByBundle\nbundleId=REG-PC-BF2142-UNLOCK-1\n")
        cmd, word, ent = c.recv()
        check((cmd, word, ent.get("errorCode")) == ("subs", 0x80000004, "99"), "not implemented error")

        # Goodbye closes the connection
        c.send("fsys", 0x80000005, "TXN=Goodbye\nreason=GOODBYE_CLIENT_NORMAL\n")
        check(c.recv() is None, "connection closed after Goodbye")
        c.close()

        # wrong password on a second connection
        c = Client(port, use_ssl)
        c.send("fsys", 0x80000001, HELLO)
        c.recv()
        c.recv()
        c.send("acct", 0x80000002, LOGIN.format(password="wrong-password"))
        cmd, word, login = c.recv()
        check(login.get("errorCode") == "122" and login.get("errorContainer.0.fieldName") == "password",
              "wrong password error")
        c.close()

        if extra:
            extra(port, use_ssl)

        # the server is still alive
        check(server.poll() is None, "server still running")
    finally:
        server.terminate()
        server.wait(timeout=5)

    with open(log) as f:
        text = f.read()
    check("logged in as Tema567" in text, "login logged")
    check("123456" not in text and "wrong-password" not in text, "passwords not logged")
    return text


def main():
    tmp = tempfile.mkdtemp()
    cert = os.path.join(tmp, "cert.pem")
    accounts = os.path.join(tmp, "accounts.txt")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=fesl", "-keyout", cert, "-out", cert],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(accounts, "w") as f:
        f.write("Tema567 123456 10\n")

    for method in ("tls", "none"):
        before = failures
        text = run(method, cert, ["-a", accounts], os.path.join(tmp, "server-%s.log" % method))
        if failures != before:
            print(text)
        print("server test (-X %s):" % method, "%d failures" % (failures - before) if failures != before else "ok")

    if os.environ.get("FESL_DB_NAME"):
        before = failures
        text = run_db(cert, os.path.join(tmp, "server-db.log"))
        if failures != before:
            print(text)
        print("server test (database):", "%d failures" % (failures - before) if failures != before else "ok")
    else:
        print("server test (database): skipped, FESL_DB_NAME not set")
    return 1 if failures else 0


TEST_ACCOUNTS = ("Tema567", "fesl_test_other")


def sql(query):
    out = subprocess.run(shlex.split(os.environ["DB_CMD"]) + ["-N", "-B"], input=query,
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("SQL failed: %s\n%s" % (query, out.stderr))
    return [line.split("\t") for line in out.stdout.splitlines()]


def run_db(cert, log):
    names = ",".join("'%s'" % n for n in TEST_ACCOUNTS)
    sql("DELETE FROM account WHERE account IN (%s)" % names)
    for name, password in zip(TEST_ACCOUNTS, ("123456", "other-pass")):
        sql("INSERT INTO account (account, password, email, countryCode, DOBYear, DOBMonth, DOBDay, "
            "zipCode, eaMailFlag, thirdPartyMailFlag) VALUES ('%s', '%s', 'test@example.com', 'US', "
            "1990, 1, 1, '', 0, 0)" % (name, password))
    user_id = sql("SELECT profileid FROM account WHERE account = 'Tema567'")[0][0]
    user = os.environ.get("FESL_DB_USER", "")

    def db_checks(port, use_ssl):
        # names are matched case-insensitively, the stored spelling is returned
        answer = login(port, use_ssl, "tema567", "123456")
        check(answer.get("displayName") == "Tema567" and answer.get("userId") == user_id,
              "case-insensitive login: %r" % answer)
        answer = login(port, use_ssl, "fesl_test_other", "other-pass")
        check("errorCode" not in answer and answer.get("userId") != user_id, "second account: %r" % answer)

        # the name is a query parameter, so SQL in it is just a wrong name
        for name in ("x' OR '1'='1", "Tema567' -- ", "Tema567\\", "A" * 46):
            answer = login(port, use_ssl, name, "123456")
            check(answer.get("errorCode") == "101", "unknown name %r: %r" % (name, answer))

        # after the database drops the server's connection it reconnects
        rows = sql("SELECT id FROM information_schema.processlist WHERE user = '%s' "
                   "AND command = 'Sleep'" % user)
        check(len(rows) >= 1, "the server's database connection is open")
        for row in rows:
            sql("KILL %s" % row[0])
        answer = login(port, use_ssl, "Tema567", "123456")
        check(answer.get("userId") == user_id, "login after the connection was killed: %r" % answer)

    env = dict(os.environ)
    try:
        # the password comes from FESL_DB_PASS, not the command line
        source = ["-dbhost", os.environ.get("FESL_DB_HOST", "127.0.0.1"),
                  "-dbport", os.environ.get("FESL_DB_PORT", "3306"),
                  "-dbname", os.environ["FESL_DB_NAME"], "-dbuser", user]
        return run("tls", cert, source, log, user_id=user_id, env=env, extra=db_checks)
    finally:
        sql("DELETE FROM account WHERE account IN (%s)" % names)


if __name__ == "__main__":
    sys.exit(main())

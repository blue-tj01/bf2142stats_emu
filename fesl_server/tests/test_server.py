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


def logged_in(port, use_ssl, name, password):
    """A client after Hello and Login, with the Login answer."""
    c = Client(port, use_ssl)
    c.send("fsys", 0x80000001, HELLO)
    c.recv()
    c.recv()
    c.send("acct", 0x80000002, LOGIN.replace("name=Tema567", "name=" + name).format(password=password))
    return c, c.recv()[2]


def run(method, cert, source, log, user_id="10", sub_id=None, env=None, extra=None):
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

        # the rest of the captured login: unlock bundle, soldiers, GameSpy pre-auth
        c.send("subs", 0x80000004, "TXN=GetEntitlementByBundle\nbundleId=REG-PC-BF2142-UNLOCK-1\n")
        cmd, word, ent = c.recv()
        check((cmd, word, ent.get("errorCode")) == ("subs", 0x80000004, "3012"), "unlock bundle: %r" % ent)
        c.send("acct", 0x80000009, "TXN=GetSubAccounts\n")
        cmd, word, subs = c.recv()
        listed = [k for k in subs if k.startswith("subAccounts.") and k != "subAccounts.[]"]
        check(word == 0x80000009 and subs.get("subAccounts.0") == "Tema567"
              and subs.get("subAccounts.[]") == str(len(listed)), "GetSubAccounts: %r" % subs)
        c.send("acct", 0x8000000a, "TXN=LoginSubAccount\nname=Tema567\n")
        cmd, word, sub = c.recv()
        check(word == 0x8000000a and sub.get("profileId") == (sub_id or user_id) and sub.get("userId") == user_id
              and len(sub.get("lkey", "")) == 28 and sub["lkey"] != login["lkey"], "LoginSubAccount: %r" % sub)
        c.send("acct", 0x8000000c, "TXN=GameSpyPreAuth\n")
        cmd, word, preauth = c.recv()
        check(word == 0x8000000c and len(preauth.get("challenge", "")) == 8
              and len(preauth.get("ticket", "")) == 90, "GameSpyPreAuth: %r" % preauth)

        # not implemented yet
        c.send("rank", 0x8000000d, "TXN=GetStats\n")
        cmd, word, stats = c.recv()
        check((cmd, word, stats.get("errorCode")) == ("rank", 0x8000000d, "99"), "not implemented error")

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
    check("logged in as Tema567" in text and "playing as Tema567" in text, "login logged")
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
NEW_ACCOUNT = "fesl_test_new"       # created by the test through AddAccount


def sql(query):
    out = subprocess.run(shlex.split(os.environ["DB_CMD"]) + ["-N", "-B"], input=query,
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("SQL failed: %s\n%s" % (query, out.stderr))
    return [line.split("\t") for line in out.stdout.splitlines()]


def run_db(cert, log):
    names = ",".join("'%s'" % n for n in TEST_ACCOUNTS + (NEW_ACCOUNT,))

    def remove_new_account():
        for row in sql("SELECT profileid FROM account WHERE account = '%s'" % NEW_ACCOUNT):
            for sub in sql("SELECT id FROM subaccount WHERE profileid = %s" % row[0]):
                sql("DELETE FROM gamespy_players WHERE id = %s" % sub[0])
            sql("DELETE FROM subaccount WHERE profileid = %s" % row[0])

    remove_new_account()
    sql("DELETE FROM account WHERE account IN (%s)" % names)
    # Tema567 gets the profile of the captured GetAccount; the other account
    # has no zip code (NULL) and a newline in its e-mail
    sql("INSERT INTO account (account, password, email, countryCode, DOBYear, DOBMonth, DOBDay, "
        "zipCode, eaMailFlag, thirdPartyMailFlag) VALUES ('Tema567', '123456', 'tema567@list.ru', "
        "'RU', 1991, 1, 7, '624760', 1, 1)")
    sql("INSERT INTO account (account, password, email, countryCode, DOBYear, DOBMonth, DOBDay, "
        "zipCode, eaMailFlag, thirdPartyMailFlag) VALUES ('fesl_test_other', 'other-pass', "
        "CONCAT('a', CHAR(10), 'b@example.com'), 'US', 1990, 12, 31, NULL, 0, 1)")
    user_id = sql("SELECT profileid FROM account WHERE account = 'Tema567'")[0][0]
    other_id = sql("SELECT profileid FROM account WHERE account = 'fesl_test_other'")[0][0]
    user = os.environ.get("FESL_DB_USER", "")

    # Tema567 has two soldiers, only the first one a GameSpy player; the
    # other account has none
    sql("DELETE FROM subaccount WHERE profileid IN (%s, %s)" % (user_id, other_id))
    sql("INSERT INTO subaccount (profileid, subaccount) VALUES (%s, 'Tema567'), (%s, 'TemaTwo')" % (user_id, user_id))
    soldiers = sql("SELECT id, subaccount FROM subaccount WHERE profileid = %s ORDER BY id" % user_id)
    sub_id, second_id = soldiers[0][0], soldiers[1][0]
    sql("DELETE FROM gamespy_players WHERE id IN (%s, %s)" % (sub_id, second_id))
    sql("INSERT INTO gamespy_players (id, email, password, passwordenc, nick) "
        "VALUES (%s, 'tema567@list.ru', '123456', 'x', 'Tema567')" % sub_id)

    def db_checks(port, use_ssl):
        # names are matched case-insensitively, the stored spelling is returned
        answer = login(port, use_ssl, "tema567", "123456")
        check(answer.get("displayName") == "Tema567" and answer.get("userId") == user_id,
              "case-insensitive login: %r" % answer)
        answer = login(port, use_ssl, "fesl_test_other", "other-pass")
        check("errorCode" not in answer and answer.get("userId") != user_id, "second account: %r" % answer)

        # GetAccount answers from the table, in the order of the capture
        c = Client(port, use_ssl)
        c.send("fsys", 0x80000001, HELLO)
        c.recv()
        c.recv()
        c.send("acct", 0x80000002, LOGIN.format(password="123456"))
        c.recv()
        c.send("acct", 0x8000000b, "TXN=GetAccount\n")
        cmd, word, account = c.recv()
        c.close()
        expected = {"TXN": "GetAccount", "name": "Tema567", "profileID": user_id, "userId": user_id,
                    "email": "tema567@list.ru", "countryCode": "RU", "countryDesc": "RU",
                    "dobDay": "7", "dobMonth": "1", "dobYear": "1991", "zipCode": "624760",
                    "gender": "U", "eaMailFlag": "1", "thirdPartyMailFlag": "1"}
        check((cmd, word) == ("acct", 0x8000000b) and account == expected
              and list(account) == list(expected), "GetAccount: %r" % account)

        c = Client(port, use_ssl)
        c.send("fsys", 0x80000001, HELLO)
        c.recv()
        c.recv()
        c.send("acct", 0x80000002, LOGIN.replace("Tema567", "fesl_test_other").format(password="other-pass"))
        c.recv()
        c.send("acct", 0x80000003, "TXN=GetAccount\n")
        account = c.recv()[2]
        c.close()
        check(account.get("zipCode") == "" and account.get("email") == "a%0ab@example.com"
              and account.get("dobDay") == "31" and len(account) == 14, "GetAccount, NULL zip: %r" % account)

        # the soldiers come from the subaccount table, in id order
        c, _ = logged_in(port, use_ssl, "Tema567", "123456")
        c.send("acct", 0x80000009, "TXN=GetSubAccounts\n")
        subs = c.recv()[2]
        check(subs == {"TXN": "GetSubAccounts", "subAccounts.0": "Tema567", "subAccounts.1": "TemaTwo",
                       "subAccounts.[]": "2"} and list(subs)[-1] == "subAccounts.[]", "GetSubAccounts: %r" % subs)

        # the pre-auth is saved where fesl_login_server.exe's GameSpy login looks
        c.send("acct", 0x8000000a, "TXN=LoginSubAccount\nname=tema567\n")
        sub = c.recv()[2]
        check(sub.get("profileId") == sub_id and sub.get("userId") == user_id, "LoginSubAccount: %r" % sub)
        c.send("acct", 0x8000000c, "TXN=GameSpyPreAuth\n")
        preauth = c.recv()[2]
        row = sql("SELECT challenge, authtoken FROM gamespy_players WHERE id = %s" % sub_id)
        check(row == [[preauth.get("challenge"), preauth.get("ticket")]], "saved pre-auth %r, answer %r" % (row, preauth))

        # a soldier without a GameSpy player still gets an answer, the log tells
        c.send("acct", 0x8000000d, "TXN=LoginSubAccount\nname=TemaTwo\n")
        sub = c.recv()[2]
        check(sub.get("profileId") == second_id, "second soldier: %r" % sub)
        c.send("acct", 0x8000000e, "TXN=GameSpyPreAuth\n")
        preauth = c.recv()[2]
        check(len(preauth.get("ticket", "")) == 90, "pre-auth without a GameSpy player: %r" % preauth)
        c.close()

        c, _ = logged_in(port, use_ssl, "fesl_test_other", "other-pass")
        c.send("acct", 0x80000009, "TXN=GetSubAccounts\n")
        subs = c.recv()[2]
        c.send("acct", 0x8000000a, "TXN=LoginSubAccount\nname=Tema567\n")     # not this account's
        sub = c.recv()[2]
        c.close()
        check(subs == {"TXN": "GetSubAccounts", "subAccounts.[]": "0"}, "no soldiers: %r" % subs)
        check(sub.get("errorCode") == "101", "another account's soldier: %r" % sub)

        # AddAccount creates the row, then the account can log in and get soldiers
        c = Client(port, use_ssl)
        c.send("fsys", 0x80000001, HELLO)
        c.recv()
        c.recv()
        new_account = ("TXN=AddAccount\nname=%s\npassword=newpass1\nemail=new@example.com\ncountryCode=DE\n"
                       "zipCode=10115\nDOBDay=3\nDOBMonth=4\nDOBYear=1985\neaMailFlag=0\nthirdPartyMailFlag=1\n")
        c.send("acct", 0x80000002, new_account % NEW_ACCOUNT)
        added = c.recv()[2]
        c.send("acct", 0x80000003, new_account % NEW_ACCOUNT.upper())
        again = c.recv()[2]
        c.send("acct", 0x80000004, (new_account % "fesl_test_bad").replace("newpass1", "x"))
        bad = c.recv()[2]
        c.close()
        row = sql("SELECT profileid, account, password, email, countryCode, DOBYear, DOBMonth, DOBDay, zipCode, "
                  "eaMailFlag, thirdPartyMailFlag FROM account WHERE account = '%s'" % NEW_ACCOUNT)
        check(len(row) == 1 and row[0][1:] == [NEW_ACCOUNT, "newpass1", "new@example.com", "DE", "1985", "4", "3",
                                               "10115", "0", "1"], "created account row: %r" % row)
        new_id = row[0][0] if row else "?"
        check(added == {"TXN": "AddAccount", "userId": new_id, "profileId": new_id}, "AddAccount: %r" % added)
        check(again.get("errorCode") == "160", "AddAccount of a taken name: %r" % again)
        check(bad.get("errorCode") == "21" and bad.get("errorContainer.0.fieldName") == "Account.Password",
              "AddAccount with a bad password: %r" % bad)
        check(not sql("SELECT 1 FROM account WHERE account = 'fesl_test_bad'"), "no row for a refused account")

        c, new_login = logged_in(port, use_ssl, NEW_ACCOUNT, "newpass1")
        check(new_login.get("userId") == new_id, "login of the new account: %r" % new_login)
        answers = []
        for n, name in enumerate(("FtSoldierA", "FtSoldierB", "FtSoldierC", "FtSoldierD", "FtSoldierE")):
            c.send("acct", 0x80000010 + n, "TXN=AddSubAccount\nname=%s\n" % name)
            answers.append(c.recv()[2].get("errorCode"))
        c.send("acct", 0x80000020, "TXN=AddSubAccount\nname=tema567\n")      # Tema567's soldier and GameSpy nick
        taken = c.recv()[2]
        check(answers == ["182"] * 4 + ["163"], "AddSubAccount up to four: %r" % answers)
        check(taken.get("errorCode") == "160", "AddSubAccount of a taken name: %r" % taken)
        soldiers = sql("SELECT id, subaccount FROM subaccount WHERE profileid = %s ORDER BY id" % new_id)
        check([s[1] for s in soldiers] == ["FtSoldierA", "FtSoldierB", "FtSoldierC", "FtSoldierD"],
              "created soldiers: %r" % soldiers)

        # DisableSubAccount removes the soldier and its GameSpy player
        if soldiers:
            sql("INSERT INTO gamespy_players (id, email, password, passwordenc, nick) "
                "VALUES (%s, 'new@example.com', 'newpass1', 'x', 'FtSoldierA')" % soldiers[0][0])
        c.send("acct", 0x80000021, "TXN=DisableSubAccount\nname=ftsoldiera\n")
        disabled = c.recv()[2]
        c.send("acct", 0x80000022, "TXN=DisableSubAccount\nname=Tema567\n")     # not this account's
        foreign = c.recv()[2]
        c.send("acct", 0x80000023, "TXN=GetSubAccounts\n")
        subs = c.recv()[2]
        c.close()
        check(disabled == {"TXN": "DisableSubAccount"}, "DisableSubAccount: %r" % disabled)
        check(foreign.get("errorCode") == "101", "DisableSubAccount of another account's soldier: %r" % foreign)
        check(subs.get("subAccounts.[]") == "3" and "FtSoldierA" not in subs.values(), "after disabling: %r" % subs)
        if soldiers:
            check(not sql("SELECT 1 FROM subaccount WHERE id = %s" % soldiers[0][0])
                  and not sql("SELECT 1 FROM gamespy_players WHERE id = %s" % soldiers[0][0]),
                  "disabled soldier's rows removed")
        check(sql("SELECT COUNT(*) FROM subaccount WHERE subaccount = 'Tema567'") == [["1"]],
              "the other account's soldier is untouched")

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
        text = run("tls", cert, source, log, user_id=user_id, sub_id=sub_id, env=env, extra=db_checks)
        check("no gamespy_players row with id %s" % second_id in text, "missing GameSpy player logged")
        return text
    finally:
        remove_new_account()
        sql("DELETE FROM gamespy_players WHERE id IN (%s, %s)" % (sub_id, second_id))
        sql("DELETE FROM subaccount WHERE profileid IN (%s, %s)" % (user_id, other_id))
        sql("DELETE FROM account WHERE account IN (%s)" % names)


if __name__ == "__main__":
    sys.exit(main())

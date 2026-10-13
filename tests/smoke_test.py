#!/usr/bin/env python3
"""End-to-end smoke test for the web stats server.

Uploads end-of-round snapshots the way a game server does, then calls every
game endpoint the way the client does, and checks the responses and the
database.

Environment:
  BASE_URL  stats server URL (default http://127.0.0.1:8088)
  WEB_DIR   path to web/, used to generate auth tokens (default ../web)
  DB_CMD    command that runs SQL read from stdin against the stats database,
            e.g. "mysql -h127.0.0.1 -uroot -proot bf2142"
"""
import os
import re
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BASE_URL = os.environ.get("BASE_URL", "http://127.0.0.1:8088").rstrip("/")
WEB_DIR = os.environ.get("WEB_DIR", os.path.join(HERE, "..", "web"))
DB_CMD = shlex.split(os.environ["DB_CMD"])

ALPHA = 10000101
ONEIL = 10000102
PIDS = (ALPHA, ONEIL)

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def sql(query):
    out = subprocess.run(DB_CMD + ["-N", "-B"], input=query, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("SQL failed: %s\n%s" % (query, out.stderr))
    return [line.split("\t") for line in out.stdout.splitlines()]


def token(pid, is_server=0, age=0):
    return subprocess.check_output(
        ["php", os.path.join(HERE, "mktoken.php"), WEB_DIR, str(pid), str(is_server), str(age)],
        text=True,
    )


def request(path, params=None, data=None):
    url = BASE_URL + "/" + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    with urllib.request.urlopen(url, data=data, timeout=30) as resp:
        return resp.read().decode("latin-1")


def framed(body):
    """Responses end in "$\\t<n>\\t$", n = length of the data without tabs/newlines."""
    m = re.match(r"^(.*)\n\$\t(\d+)\t\$\n?$", body, re.S)
    return bool(m) and int(m.group(2)) == len(re.sub(r"[\t\n]", "", m.group(1)))


def snapshot(nick_two="O'Neil", blank=False):
    now = int(time.time())

    def player(i, pid, nick, rank):
        keys = [
            ("ban", 0), ("c", 1), ("capa", 2), ("cpt", 3), ("crpt", 500), ("cs", 10), ("dass", 1),
            ("dcpt", 1), ("dstrk", 2), ("dths", 5), ("gsco", 400), ("hls", 3), ("kick", 0),
            ("klla", 2), ("klls", 12), ("klstrk", 4), ("kluav", 0), ("nick", nick), ("ncpt", 1),
            ("pdt", 1), ("pdtc", 2), ("pid", pid), ("resp", 1), ("rnk", rank), ("rnkcg", 0),
            ("rps", 1), ("rvs", 1), ("slbspn", 1), ("sluav", 0), ("suic", 0), ("tac", 60),
            ("talw", 300), ("tas", 5), ("tasl", 100), ("tasm", 200), ("tcd", 1), ("tcrd", 0),
            ("tdmg", 0), ("tdrps", 0), ("tds", 2), ("tgd", 0), ("tgr", 0), ("tkls", 0),
            ("toth", 50), ("tots", 200), ("tt", 600), ("tvdmg", 0), ("twsc", 30), ("t", 1 + i % 2),
            ("mvns", ONEIL if pid == ALPHA else ALPHA), ("mvks", 3),
            ("vdstry-0", 1), ("vdths-0", 0), ("vkls-0", 2), ("vrkls-0", 0), ("vtp-0", 120),
            ("vbf-0", 10), ("vbh-0", 4),
            ("kdths-0", 1), ("kdths-1", 2), ("kdths-2", 1), ("kdths-3", 1), ("kkls-0", 3),
            ("kkls-1", 4), ("kkls-2", 3), ("kkls-3", 2), ("ktt-0", 100), ("ktt-1", 200),
            ("ktt-2", 150), ("ktt-3", 150),
            ("wdths-0", 1), ("wkls-0", 5), ("waccu-0", "0.25"), ("wtp-0", 300), ("wbf-0", 100),
            ("wbh-0", 25),
        ]
        if blank:
            keys = [(k, v if k in ("pid", "nick") else "") for k, v in keys]
        return "\\".join("%s_%d\\%s" % (k, i, v) for k, v in keys)

    head = "BF2142\\Suez_Canal\\mapstart\\%d\\mapend\\%d\\win\\1\\gm\\0\\m\\2\\v\\bf2142\\pc\\2\\rwa\\1\\" % (
        now - 600, now)
    return (head + player(0, ALPHA, "Alpha", 3) + "\\" + player(1, ONEIL, nick_two, 1) + "\\EOF\\1").encode("latin-1")


def reset_db():
    ids = ",".join(str(p) for p in PIDS)
    stmts = ["DELETE FROM subaccount WHERE id IN (%s)" % ids]
    for table in ("playerprogress", "stats_a", "stats_e", "stats_m", "stats_v", "stats_w", "awards", "unlocks"):
        stmts.append("DELETE FROM `%s` WHERE pid IN (%s)" % (table, ids))
    stmts.append("INSERT INTO subaccount (id, profileid, subaccount) VALUES (%d, %d, 'Alpha'), (%d, %d, 'O%%27Neil')"
                 % (ALPHA, ALPHA, ONEIL, ONEIL))
    sql(";\n".join(stmts) + ";")


def test_round_upload():
    print("Round upload (bf2142statistics.php)")
    body = request("bf2142statistics.php", data=snapshot())
    check("D\tOK\tsuez_canal\t" in body, "snapshot accepted")
    check(body.count("$\tOK\t$") == 1, "response sent exactly once")

    rows = dict((int(r[0]), r[1:]) for r in sql(
        "SELECT pid, nick, gsco FROM playerprogress WHERE pid IN (%d, %d)" % PIDS))
    check(set(rows) == set(PIDS), "both players saved")
    check(rows.get(ONEIL, [None])[0] == "O'Neil", "nickname with an apostrophe saved intact")
    check(rows.get(ALPHA, [None, None])[1] == "400", "score saved")

    body = request("bf2142statistics.php", data=snapshot(blank=True))
    check("D\tOK\tsuez_canal\t" in body, "snapshot with blank stat values accepted")
    rows = sql("SELECT gsco FROM playerprogress WHERE pid = %d" % ALPHA)
    check(rows and rows[0][0] == "400", "blank values counted as 0")


def test_endpoints():
    auth = token(ALPHA)
    calls = [
        ("getawardsinfo.aspx", {"pid": ALPHA}),
        ("getbackendinfo.aspx", {}),
        ("getunlocksinfo.aspx", {}),
        ("playersearch.aspx", {"nick": "Neil"}),
        ("ValidatePlayer.aspx", {"pid": ALPHA, "SoldierNick": "Alpha"}),
    ]
    # Not covered: the "supremecommander" leaderboard is a hardcoded placeholder
    # with a fixed length, and getplayerinfo has no "award" mode (awards come
    # from getawardsinfo.aspx).
    for t in ("overallscore", "combatscore", "commanderscore", "teamworkscore", "efficiency",
              "risingstar"):
        calls.append(("getleaderboard.aspx", {"pos": 1, "after": 17, "type": t}))
    for t in ("weapon", "vehicle"):
        calls.append(("getleaderboard.aspx", {"pos": 1, "after": 17, "type": t, "id": 0}))
    for mode in ("base", "ovr", "ply", "titan", "wrk", "com", "wep", "veh", "map"):
        calls.append(("getplayerinfo.aspx", {"mode": mode, "pid": ALPHA}))
    for mode in ("point", "score", "ttp", "kills", "spm", "role", "flag", "waccu", "wl", "twsc", "sup"):
        calls.append(("getplayerprogress.aspx", {"mode": mode, "scale": "game"}))

    print("Endpoints return well-formed responses")
    responses = {}
    for path, params in calls:
        p = dict(params, auth=auth)
        body = request(path, p)
        responses.setdefault(path, []).append(body)
        label = path + " " + " ".join("%s=%s" % kv for kv in params.items() if kv[0] in ("type", "mode"))
        check(body.startswith("O\n") and framed(body), label.strip())

    print("Endpoint content")
    check("D\t%d\tO'Neil" % ONEIL in responses["playersearch.aspx"][0], "player search finds O'Neil")
    check("H\tdate\twins\tlosses" in responses["getplayerprogress.aspx"][8], "progress wl header")
    unlocks = responses["getunlocksinfo.aspx"][0]
    m = re.search(r"H\tAvcred\nD\t(\d+)", unlocks)
    check(bool(m) and int(m.group(1)) > 0, "unlock credits granted for rank")

    body = request("selectunlock.aspx", {"auth": auth, "uid": 111})
    check("E\t" not in body, "select an unlock")
    body = request("getunlocksinfo.aspx", {"auth": auth})
    check("\nD\t111" in body, "selected unlock is listed")


def test_tokens():
    print("Auth tokens")
    old_client = token(ALPHA, 0, 1000)
    old_server = token(ALPHA, 1, 1000)
    params = {"mode": "base", "pid": ALPHA}
    check(request("getplayerinfo.aspx", dict(params, auth=old_client)).startswith("ExpiredAuth"),
          "expired player token rejected")
    check(not request("getplayerinfo.aspx", dict(params, auth=old_server)).startswith("ExpiredAuth"),
          "old game-server token still accepted")
    check(request("getleaderboard.aspx", {"auth": old_client, "pos": 1, "after": 17, "type": "overallscore"})
          .startswith("ExpiredAuth"), "expired token rejected by leaderboard")


def test_no_request_dumps():
    print("Request logging is off by default")
    for name in ("getplayerinfo.txt", "getunlocksinfo.txt", "getleaderboard.txt", "sinthetixData.txt"):
        try:
            request(name)
            status = 200
        except urllib.error.HTTPError as e:
            status = e.code
        # 404 when the file doesn't exist; Apache's .htaccess answers 403 for
        # any .txt name, existing or not
        check(status in (403, 404), "no %s downloadable from the web root" % name)


def wait_for_server():
    for _ in range(60):
        try:
            request("ValidatePlayer.aspx")
            return
        except OSError:
            time.sleep(1)
    sys.exit("Stats server at %s did not come up" % BASE_URL)


if __name__ == "__main__":
    wait_for_server()
    reset_db()
    test_round_upload()
    test_endpoints()
    test_tokens()
    test_no_request_dumps()
    if failures:
        print("\n%d check(s) failed" % len(failures))
        sys.exit(1)
    print("\nAll checks passed")

# Real-game test and capture plan

This plan records how the real Battlefield 2142 client and dedicated server talk to the login services. The recordings become test cases for the open FESL server, the same way the two captures in [`stuff/pcap/`](../stuff/pcap) did. It has three parts:

| Phase | Goal | Against |
| --- | --- | --- |
| [A](#phase-a-reference-recordings-with-the-closed-server) | Reference recordings of every login, account and soldier action, and of GameSpy | `fesl_login_server.exe` (known to work) |
| [B](#phase-b-the-open-fesl-server-with-the-real-game) | Check the open FESL server with the real client, step by step | `fesl_server/` for FESL, `fesl_login_server.exe` for GameSpy |
| [C](#phase-c-gamespy-in-detail) | Record each GameSpy service in enough detail to reimplement it | `fesl_login_server.exe`, the web stats server, a dedicated server |

Every step says what to record and what to send back. [Sending the results](#sending-the-results) explains how to package them.

## Contents

- [Lab setup](#lab-setup)
- [Recording tools](#recording-tools)
- [Phase A: reference recordings with the closed server](#phase-a-reference-recordings-with-the-closed-server)
- [Phase B: the open FESL server with the real game](#phase-b-the-open-fesl-server-with-the-real-game)
- [Phase C: GameSpy in detail](#phase-c-gamespy-in-detail)
- [Sending the results](#sending-the-results)
- [Troubleshooting](#troubleshooting)

## Lab setup

### Machines

Use at least two machines or VMs on one LAN, so the traffic between them can be recorded. The addresses below are examples; write down your real ones in the [run sheet](#run-sheet).

| Name | OS | Runs | Example IP |
| --- | --- | --- | --- |
| **SRV** | Windows (a VM is fine) | `fesl_login_server.exe`, MySQL/MariaDB and the web stats server (or the Docker stack), the Windows dedicated server, Wireshark | `192.168.1.3` |
| **LNX** | Linux | the open `fesl_server`, `stcppipe`, `tcpdump`, later the Linux dedicated server | `192.168.1.4` |
| **CLI** | Windows | the BF2142 client, Wireshark | `192.168.1.10` |

> [!TIP]
> The patched client has the server address `192.168.1.3` built in (see the main README). Giving SRV that address means Phase A needs no hex-editing.

### Software

- **SRV:** set up as in the main [README](../../README.md#setup) (database, login server, web server, game server). Start the login server with logging and dumps on (see [Recording tools](#recording-tools)).
- **LNX:** build the open server and stcppipe with SSLv3, the way the game connects:
  ```sh
  cd fesl_server/src && make ssl3          # stcppipe
  cd server && make ssl3                   # fesl_server, with database support if the MySQL/MariaDB client library is installed
  openssl req -x509 -newkey rsa:1024 -nodes -days 365 -subj /CN=fesl -keyout cert.pem -out cert.pem
  ```
- **CLI:** BF2142 1.51 with `cracked_exe/1.51/BF2142.exe`, [Wireshark](https://www.wireshark.org/) with Npcap. Keep a copy of the executable for Phase B (see below).
- Run the game **windowed** if you can, so you can watch Wireshark and the game together.

### Accounts

Use only throwaway accounts with throwaway passwords. Recordings contain the login password in clear text, and so does the database. Suggested names, so they're easy to find in the recordings:

| Account | Password | Soldiers |
| --- | --- | --- |
| `fesltest1` | `testpass1` | created during the test (`Soldier1A`, `Soldier1B`) |
| `fesltest2` | `testpass2` | none, for the "no soldiers" case |

### Pointing the game at the servers

The patched 1.51 client connects to the address built into the executable, `192.168.1.3` unless you change it (see the main README). Scenario A0 shows which servers it reaches through that address and which host names it still looks up. Point those names at the right machine in the hosts file (`C:\Windows\System32\drivers\etc\hosts`; the main README says `hosts.ics`, so use whichever works on your Windows). Run `ipconfig /flushdns` after each change, and check with `ping <name>` that the name resolves to the IP you meant.

For **Phase B**, FESL must reach LNX while GameSpy and the web stats stay on SRV:

- Hex-edit the **copy** of the executable to LNX's address, as the main README describes, and use it only in Phase B.
- If A0 shows that GameSpy and the stats also go to the built-in address, forward them from LNX to SRV, for example with `socat`:
  ```sh
  for p in 28910 29900 29901 80; do socat TCP-LISTEN:$p,fork,reuseaddr TCP:<SRV IP>:$p & done
  socat UDP-LISTEN:27900,fork,reuseaddr UDP:<SRV IP>:27900 &
  ```
  Add any other port A0 shows.

| Host name | Phase A | Phase B |
| --- | --- | --- |
| `bf2142-pc.fesl.ea.com` | SRV | **LNX** |
| `gpcm.gamespy.com`, `gpsp.gamespy.com`, `stella.available.gamespy.com`, `stella.ms5.gamespy.com`, `stella.master.gamespy.com` | SRV | SRV |
| `stella.prod.gamespy.com`, `gamestats.gamespy.com`, `eapusher.dice.se`, `eapusher.dise.se` | SRV | SRV |
| `ea.com`, `gamespy.com`, `fesl.ea.com`, `messaging.ea.com` | SRV | SRV |

On **SRV**, the dedicated server needs `stella.prod.gamespy.com` and `stella.master.gamespy.com` pointing at SRV, as in the main README.

## Recording tools

Start all of them **before** the game, and stop them only after the game has quit, so the first and last packets are included.

| What | Where | How |
| --- | --- | --- |
| Decrypted FESL of the closed server | SRV | `fesl_login_server -v -l 1 -d dumps ...`: `-d` writes every connection, decrypted, as capture files into `dumps\`, and `-l 1` writes logs per port (`logs_18300\`, `logs_29900\`, ...) |
| Decrypted FESL of any server | LNX | `stcppipe -S -X ssl3 "" "" -d dumps <server_ip> <server_port> 18300`, between the game and the server (see [Phase B](#b1-start-the-servers)) |
| All traffic of the game | CLI | Wireshark on the LAN adapter, capture filter `host 192.168.1.3 or host 192.168.1.4 or port 53` (use your IPs), saved as `.pcapng` |
| All traffic of a server | SRV / LNX | Wireshark on SRV; on LNX `sudo tcpdump -i any -w lnx.pcap 'not port 22'` |
| Open server log | LNX | `./fesl_server ... 2>&1 \| tee fesl_server.log` |
| Web stats requests | SRV | `$log_requests = 1` in `_ccconfig.php` (`LOG_REQUESTS=1` with Docker), see the main README's [Logs](../../README.md#logs) |
| Game log and screens | CLI | Any error messages and screenshots of them, with the time |

> [!NOTE]
> FESL is SSL-encrypted on the wire, so the Wireshark recordings only show its timing. The decrypted content comes from `-d` or stcppipe. GameSpy and the web stats traffic are plain or only lightly encoded, so Wireshark records them in full.

Write down the **wall-clock time** at the start of each scenario. It's the easiest way to match the recordings from the different machines afterwards.

## Phase A: reference recordings with the closed server

Point every host name at SRV and run the scenarios below with the 1.51 client. Use a new dump folder and a new Wireshark file for each scenario, named after its ID (`A3.pcapng`, `dumps-A3\`, ...).

| ID | Scenario | Steps | What it gives us |
| --- | --- | --- | --- |
| A0 | Discover host names | Before editing the hosts file, start Wireshark without a filter, start the game and wait for the online menu to fail. Save it. | Every host name the game looks up, including any not in the list above, and which connections go straight to the built-in address |
| A1 | Create an account | Create `fesltest1` in the game: country, date of birth, terms of use, e-mail | `GetCountryList`, `GetTos`, `AddAccount` (the first two aren't implemented yet in the open server) |
| A2 | Log in | Quit and start again, log in as `fesltest1` | The full login, to compare with our captures |
| A3 | Create soldiers | Create `Soldier1A`, then `Soldier1B` | `AddSubAccount`, and how the `gamespy_players` row is created |
| A4 | Pick a soldier | Log in, choose `Soldier1A`, stay in the menu for 5 minutes | `LoginSubAccount`, `GameSpyPreAuth`, the GameSpy login, keep-alive Pings and MemChecks over time |
| A5 | Server browser | Open the server list, refresh it, open a server's details | The server list protocol (port 28910) and the availability check |
| A6 | Play a round | Start the dedicated server on SRV, join it from CLI, play until the round ends | The game server's own FESL and GameSpy login, heartbeats, the end-of-round stats upload, the client's stats requests |
| A7 | Account screens | View and edit the account details (e-mail, newsletter flags) if the game offers it | `GetAccount`, `UpdateAccount` |
| A8 | Delete a soldier | Delete `Soldier1B` | `DisableSubAccount` |
| A9 | Errors | Log in with a wrong password, then with an unknown name, then 6 wrong passwords in a row | The game's error messages for 122, 101 and repeated failures |
| A10 | Remember me | Tick "remember password", log in, quit, start again and log in without typing it | The `encryptedInfo` login (refused by the open server today) |
| A11 | Leaving | Quit from the menu; then, separately, close the game while logged in (Alt-F4) | `Goodbye`, and what a dropped connection looks like |
| A12 | No soldiers | Log in as `fesltest2`, which has none, and stop at the soldier screen | What the game does with an empty `GetSubAccounts` |

For **A6**, also record on SRV (or on LNX, with the Linux dedicated server), with `tcpdump`/Wireshark filtered on the dedicated server's host:

- its FESL login, which the closed server dumps with `-d`;
- the GameSpy heartbeats (UDP 27900);
- the HTTP POST of the round's stats to the web server.

The Linux dedicated server may need the FESL patch too. `lpatch` patches any file, so try `exe_patch/fesl.lpatch` on the Linux binary, and write down whether it found the bytes to patch.

## Phase B: the open FESL server with the real game

The open server answers FESL; GameSpy and the web stats stay on SRV. Both FESL servers use the **same database**, so an account and soldiers created in Phase A can log in here.

### B1. Start the servers

On **LNX**:

```sh
# the open server on 18301, stcppipe in front of it on 18300 to record the decrypted traffic
FESL_DB_PASS=... ./fesl_server -c cert.pem -p 18301 -dbhost <SRV IP> -dbname bf2142 -dbuser fesl 2>&1 | tee fesl_server.log
./stcppipe -S -X ssl3 "" "" -d dumps-B 127.0.0.1 18301 18300
```

- **Database user:** give the open server's user `SELECT`, `INSERT` on `account`, `INSERT, DELETE` on `subaccount`, and `UPDATE (challenge, authtoken), DELETE` on `gamespy_players`. See [Running the FESL server](../README.md#running-the-fesl-server).
- **Remote MySQL:** allow it from LNX only, never from the internet.

On **CLI**, use the executable copy pointed at LNX (see [Pointing the game at the servers](#pointing-the-game-at-the-servers)), keeping everything else at SRV.

> [!TIP]
> Without stcppipe, run `fesl_server` on port 18300 directly. Its log then shows every request and answer (command, TXN, number, error code), but not the values.

### B2. Steps and pass criteria

Run them in order. Stop at the first failure and send what you have.

| ID | Steps | Pass when | If it fails, send |
| --- | --- | --- | --- |
| B1 | Start the game | The login screen appears; `fesl_server.log` shows `fsys Hello` and `MemCheck` answered | The game's message, `fesl_server.log`, the stcppipe dump |
| B2 | Log in as `fesltest1` | Accepted; the log shows `logged in as fesltest1` | The same, plus what the game showed |
| B3 | Wrong password, unknown name | The game shows an error message for each; the log shows errors 122 and 101 | The message text the game shows (we chose these codes from other games) |
| B4 | Wait at the soldier screen | It lists `Soldier1A` (and `Soldier1B` if still there) | The dump: `GetSubAccounts` and anything the game sent that the log marks with error 99 |
| B5 | Pick `Soldier1A` | The log shows `playing as Soldier1A`, then the GameSpy login on SRV succeeds and the main menu opens | Both machines' recordings, and the `gamespy_players` row: `SELECT id, nick, challenge, authtoken FROM gamespy_players WHERE nick='Soldier1A'` |
| B6 | Stay in the menu for 5 minutes | Still connected; the log shows a `fsys Ping` every 30 seconds, answered | The log and the time the game disconnected |
| B7 | Server browser, then join the dedicated server and play a round | Same as with the closed server | Everything; this mixes the open FESL server with the closed GameSpy server |
| B8 | Quit from the menu | The log shows `fsys Goodbye` and `disconnected` | The log |

Any request the open server doesn't know is answered with error 99 and shows in the log as `error 99`. Those are the next things to implement, so note which screen the game was on when it sent them.

## Phase C: GameSpy in detail

Phase A records GameSpy as a side effect. This phase isolates each service in its own short recording, so each one can be studied and reimplemented separately. On CLI, use one Wireshark file per row, with the given capture filter (replace `SRV` with its IP).

| ID | Service | Port | Capture filter | Steps |
| --- | --- | --- | --- | --- |
| C1 | Availability check (`stella.available.gamespy.com`) | 27900/UDP | `host SRV and udp port 27900` | Start the game up to the login screen |
| C2 | GameSpy login (`gpcm.gamespy.com`) | 29900/TCP | `host SRV and tcp port 29900` | Log in and pick a soldier (after the FESL `GameSpyPreAuth`); stay 2 minutes; quit |
| C3 | Profile search (`gpsp.gamespy.com`) | 29901/TCP | `host SRV and tcp port 29901` | Anything that searches players (add a buddy, if the game has it). The closed server doesn't serve this port, so note any connection attempt |
| C4 | Server list (`stella.ms5.gamespy.com`) | 28910/TCP | `host SRV and tcp port 28910` | Open the server browser with the dedicated server running, refresh twice, apply a filter |
| C5 | Server heartbeats (`stella.master.gamespy.com`) | 27900/UDP | on SRV: `udp port 27900` | Start the dedicated server, wait 5 minutes, add a bot or player, stop the server |
| C6 | Stats and unlocks (`stella.prod.gamespy.com`, HTTP) | 80/TCP | `host SRV and tcp port 80` | Open the stats and unlock screens, play a round (A6), open them again |
| C7 | Anything else | | `host SRV and not tcp port 18300` | A whole session; then list the ports used: Wireshark → Statistics → Conversations |

Notes:

- **Encoding:** `gpcm` is text with backslash-separated fields, so its login challenge and response are readable. The server list (C4) is encrypted; recording both the request and the reply is enough to decode it later.
- **Logs:** the closed server's `-l 1` log folders (`logs_27900\`, `logs_28910\`, `logs_29900\`) show its view of C1, C2, C4 and C5. Send them along.
- **Database state:** after C2 and C5, export the GameSpy rows they touch: `SELECT * FROM gamespy_players` (test accounts only) and `SELECT pid, status, last_update FROM srv_info`.

## Sending the results

Make one folder per scenario, named after its ID, for example `A4/`. Put in it:

- the Wireshark files (`.pcapng`) of every machine that recorded;
- the decrypted dumps (`dumps/` from `-d`, or stcppipe's `.acp` files);
- the logs: `fesl_server.log`, the closed server's `logs_*` folders, and the web request logs;
- a `notes.txt` with the [run sheet](#run-sheet) details, the start time, what you did, and what the game showed (screenshots are welcome).

Zip the folders and share them however suits you. I'll turn each recording into replay tests, like those for `stuff/pcap/`, and implement what's missing.

> [!WARNING]
> Before sharing: the recordings contain the test accounts' passwords, their e-mail addresses, and your LAN and public IP addresses. That's why only throwaway accounts should be used. If a recording contains anything else personal, say so in `notes.txt`, or remove that scenario.

### Run sheet

Copy this into each `notes.txt`:

```
Scenario:            (e.g. A4)
Date and start time: (with time zone)
Executable:          cracked_exe/1.51 as is, or hex-edited to which IP
SRV:                 IP, Windows version, fesl_login_server options, database (MySQL/MariaDB + version)
LNX:                 IP, distribution, fesl_server commit, build (make / make ssl3)
CLI:                 IP, Windows version
Hosts file:          which names point where
Dedicated server:    Windows/Linux, version, patched (yes/no)
What happened:
Errors shown:
```

## Troubleshooting

| Problem | Check |
| --- | --- |
| The game doesn't connect at all | The executable points at the right IP and the hosts file names resolve as intended; the firewall on SRV/LNX allows TCP 18300, 28910, 29900 and UDP 27900; the patched executable is in use |
| FESL connects, then drops right after the SSL handshake | The executable isn't patched (the certificate check is still active), or the server wasn't built with `make ssl3` (the log says `SSL handshake failed`) |
| stcppipe shows no data | Its argument order is `<server_ip> <server_port> <local_port>`; the server behind it must listen on another port than stcppipe |
| The open server can't reach the database | MySQL's `bind-address` and the user's host (`'fesl'@'<LNX IP>'`); the open server prints the database error at startup |
| `GameSpyPreAuth` is answered but the GameSpy login fails | The soldier needs a `gamespy_players` row with `id` = its `subaccount.id` (the closed server creates it); the open server logs `no gamespy_players row` otherwise |
| Times in the recordings don't line up | Sync all clocks (NTP) before starting; login tokens also expire after about 12 minutes if a clock is off |

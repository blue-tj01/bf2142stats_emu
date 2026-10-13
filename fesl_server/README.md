# Open FESL server

[![CI](https://github.com/blue-tj01/bf2142stats_emu/actions/workflows/ci.yml/badge.svg)](https://github.com/blue-tj01/bf2142stats_emu/actions/workflows/ci.yml)
[![License: GPL v2](https://img.shields.io/badge/license-GPLv2-blue.svg)](../LICENSE)

An open-source replacement for the FESL login service (`fesl.ea.com`) that **Battlefield 2142** used, and in time for the GameSpy services too. It is meant to replace the closed [`fesl_login_server.exe`](../fesl_login_server) of this repository, which can't be audited or fixed.

This folder was imported from [blue-tj01/2142master](https://github.com/blue-tj01/2142master), with its full history.

> [!NOTE]
> The patched game executables in [`cracked_exe/`](../cracked_exe) and the [`exe_patch/`](../exe_patch) patches for the dedicated server include aluigi's FESL certificate check remover. With them, the game accepts this server's self-signed certificate. The connection is still SSLv3, so build the server with `make ssl3`.

> **Status: early development.** The protocols are being reverse engineered from captured traffic. The FESL server answers every request of the captured logins, from Hello to GameSpyPreAuth, and creates and removes accounts and soldiers. GameSpy itself is not implemented yet.

## Contents

- [Roadmap](#roadmap)
- [Repository layout](#repository-layout)
- [Getting started](#getting-started)
- [Running the FESL server](#running-the-fesl-server)
- [Capturing game traffic](#capturing-game-traffic)
- [FESL protocol overview](#fesl-protocol-overview)
- [Contributing](#contributing)
- [License](#license)
- [Credits](#credits)

## Roadmap

- [x] Capture game traffic, including SSL-encrypted FESL sessions
- [x] FESL packet decoder and encoder, with tests that replay the captures
- [x] FESL server: Hello, MemCheck, Login, GetAccount, GetObjectInventory and keep-alive Ping
- [x] FESL unlock bundles, sub-accounts (soldiers) and GameSpy pre-auth
- [x] FESL account and soldier creation and removal (AddAccount, AddSubAccount, DisableSubAccount)
- [ ] FESL account updates and recovery (UpdateAccount, GetCountryList, GetTos, SendAccountName, SendPassword)
- [ ] GameSpy player rows for new soldiers (needs the `passwordenc` format `fesl_login_server.exe` uses)
- [ ] GameSpy protocols and sub-accounts
- [x] Accounts from the `account` table of the stats database (MySQL / MariaDB)
- [ ] Other database backends (PostgreSQL, SQLite3)
- [ ] Daemonization of each service
- [ ] INI configuration (minIni is already included)

## Repository layout

| Path | Contents |
| --- | --- |
| `src/server/` | FESL server (`fesl_server.c`), session handlers (`fesl_session.c`), packet decoder/encoder (`fesl.c`), accounts file (`accounts.c`) and `account` table (`accounts_db.c`) |
| `src/` | [stcppipe](http://aluigi.altervista.org/mytoolz.htm) capture proxy by Luigi Auriemma, and the minIni INI parser |
| `tests/` | Decoder and session tests that replay the captures, and a server end-to-end test |
| `stuff/pcap/` | Captured login sessions (`dump1.pcap` raw, `dump2_ssl.pcap` decrypted FESL) |
| `stuff/info/` | Archived FESL protocol notes from aluigi's forum |
| `doc/` | Licenses of the bundled third-party code |

## Getting started

You need a C compiler, `make`, and `unzip`. Building stcppipe against the system OpenSSL also needs the OpenSSL headers (`libssl-dev` on Debian/Ubuntu). For database support the server also needs the MySQL or MariaDB client library (`libmariadb-dev` or `libmysqlclient-dev`); without it, it builds with the accounts file only.

Run the tests:

```sh
make -C tests test               # decoder and session tests, replaying stuff/pcap
make -C tests test SANITIZE=1    # the same with AddressSanitizer/UBSan

make -C src/server               # server end-to-end test, needs Python 3 and openssl
cd tests && python3 test_server.py
```

To include the `account` table, point the end-to-end test at a database loaded with [`database/bf2142.sql`](../database/bf2142.sql). It adds two test accounts and removes them again:

```sh
FESL_DB_NAME=bf2142 FESL_DB_USER=fesl FESL_DB_PASS=... \
DB_CMD="mysql -h127.0.0.1 -uroot -p... bf2142" python3 test_server.py
```

CI runs all of these on every pull request and on every push to `master`, against MariaDB 10.11 and MySQL 8.0, plus the stcppipe build.

## Running the FESL server

The game connects with **SSLv3**, so build the server against the private SSLv3 OpenSSL (see [below](#capturing-game-traffic)):

```sh
cd src/server
make ssl3
openssl req -x509 -newkey rsa:1024 -nodes -days 365 -subj /CN=fesl -keyout cert.pem -out cert.pem
./fesl_server -c cert.pem -a accounts.example.txt     # listens on port 18300
```

To use the accounts of the stats database instead, the same `account` table `fesl_login_server.exe` uses, pass the database options it takes:

```sh
FESL_DB_PASS=yourpassword ./fesl_server -c cert.pem -dbhost 127.0.0.1 -dbname bf2142 -dbuser fesl
```

The server reads the tables; it writes new accounts and soldiers, removes soldiers, and saves the GameSpy pre-auth of a soldier. Give it these rights, and no more:

```sql
CREATE USER 'fesl'@'localhost' IDENTIFIED BY 'yourpassword';
GRANT SELECT ON bf2142.* TO 'fesl'@'localhost';
GRANT INSERT ON bf2142.account TO 'fesl'@'localhost';
GRANT INSERT, DELETE ON bf2142.subaccount TO 'fesl'@'localhost';
GRANT UPDATE (challenge, authtoken), DELETE ON bf2142.gamespy_players TO 'fesl'@'localhost';
```

Without the `INSERT` and `DELETE` rights, logins still work and creating accounts or soldiers answers error 99.

| Option | Meaning |
| --- | --- |
| `-c CERT` | PEM file with the certificate and its passwordless private key (not needed with `-X none`) |
| `-a ACCOUNTS` | Accounts file, one `name password user_id [status]` per line (see `accounts.example.txt`) |
| `-dbhost`, `-dbport`, `-dbname`, `-dbuser`, `-dbpass` | Read accounts from the `account` table instead (host default `127.0.0.1`). The password can also come from the `FESL_DB_PASS` environment variable, which keeps it out of the process list |
| `-p PORT` / `-b IP` | Listening port (default 18300) and address |
| `-X METHOD` | `ssl3` (default), `tls`, or `none` for plain TCP with a client patched to skip SSL. A plain `make` build has no SSLv3 |
| `-i SECONDS` | Interval between server Pings, sent even while the client is busy (default 30) |

The server answers:

- **Hello**, followed by an unsolicited MemCheck with packet number 0.
- **Login**, checked against the accounts file or the `account` table. With the database, `profileId` and `userId` are the account's `profileid`, and `displayName` is the name as stored.
- **GetAccount**, after a Login, with the profile from the `account` table: e-mail, country, date of birth, zip code and mail flags, in the order of the captured answer. The table has no gender, so it sends `U` as the capture did. Accounts from the accounts file have no profile, so those fields are empty or 0.
- **GetObjectInventory**, always with an empty inventory.
- **GetEntitlementByBundle**, for the unlock bundles `REG-PC-BF2142-UNLOCK-1` to `3`: error 3012, "The customer has never had entitlement for this bundle.", word for word as captured.
- **GetSubAccounts**, the soldiers of the account: the `subaccount` rows of its `profileid`, in id order (up to 16). An account from the accounts file has one soldier, itself, as in the capture.
- **LoginSubAccount**, for one of those soldiers (case-insensitive name): a new `lkey`, the soldier's id as `profileId` and the account's as `userId`. Any other name gets error 101.
- **AddAccount**, after Hello: a new row in `account` with the name, password, e-mail, country, date of birth, zip code and mail flags the game sends; the answer has the new `profileid` as `userId` and `profileId`. Each field is checked first; a bad one gets error 21 with BF2142's name for it (`Account.ScreenName`, `Account.Password`, `Account.EmailAddress`, `Account.Address.Country`, `Account.BirthDate`, `Account.Address.Zip`) and the reason as `fieldError` (1 required, 2 too short, 3 too long, 4 must begin with a letter, 6 invalid). A name already taken, in any case, gets error 160. The rules:
  - names: 3 to 45 characters, starting with a letter, then letters, digits and `_ - . [ ]` (names are GameSpy nicks too, which can't contain `\`);
  - passwords: 4 to 16 letters and digits, as the game's own error message says;
  - e-mail: one `@` and a `.` after it, up to 45 characters; country: up to 3 letters or digits; a real date of birth from 1900.
- **AddSubAccount**, after Login: a new soldier in `subaccount`, at most four per account (error 163, the game's "You can only have four (4) active Soldiers"). Soldier names follow the name rules, up to 20 characters (the stats' `playerprogress.nick`), and must not be used by any soldier or GameSpy player (error 160). The answer is `errorCode=182` ("no error"), as openspy, which works with BF2142, sends it.
- **DisableSubAccount**, after Login: removes one of the account's soldiers (any other name gets error 101) and its `gamespy_players` row, as `fesl_login_server.exe` does. Its stats stay.
- **GameSpyPreAuth**, after LoginSubAccount: a random `challenge` and `ticket`, the ticket the game then shows the GameSpy login. With the database they are also saved as `challenge` and `authtoken` of the soldier's `gamespy_players` row (`id` = the soldier's id), where the GameSpy login of `fesl_login_server.exe` looks for them. If that row is missing or can't be written, the server logs it and still answers, but that soldier's GameSpy login will fail.

It also sends a Ping every 30 seconds and closes connections that stay silent for 3 intervals. Any other request gets a "Not implemented" error (`errorCode=99`).

Login errors:

| Code | Meaning |
| --- | --- |
| 101 | Unknown user. Sent with `errorContainer` field `name` when the name is empty |
| 99 | The database can't be reached. The server reconnects on the next Login, and this doesn't count as a failed attempt |
| 122 | Wrong or missing password, with `errorContainer` field `password` |
| 102 / 103 / 105 | Account `disabled` / `banned` / `pending` (the optional status column of the accounts file; only revealed after the right password). The `account` table has no status, so database accounts are always active |
| 121 | Too many attempts: after 5 failed Logins on one connection |

> [!NOTE]
> - Passwords are stored in clear text, both in the accounts file and in the `account` table (as `fesl_login_server.exe` expects). Don't reuse real passwords.
> - A soldier created by this server has no `gamespy_players` row yet, so `fesl_login_server.exe`'s GameSpy login won't accept it: how that server fills `passwordenc` isn't known. Soldiers created by `fesl_login_server.exe` work.
> - The formats of AddAccount, AddSubAccount and DisableSubAccount come from [openspy-core](https://github.com/openspy/openspy-core), which serves BF2142; they aren't in our captures. openspy has no license, so only its protocol details are used, not its code.
> - The name is passed to the database as a query parameter, never pasted into the SQL.
> - Text from accounts (names, e-mails, zip codes) is sent with control characters and `%` escaped as `%XX`, so a stored value can't add fields to a packet.
> - Codes 101 and 122 are used by the BFBC2 and NFS Carbon emulators, and the others were checked against the NFS Carbon client. None has been checked against BF2142.
> - The MemCheck numbering and the fixed 30-second Ping follow EA's real NFS Carbon servers. Our BF2142 captures, made against an emulator, reused the Hello number for MemCheck.
> - An unpatched game rejects the self-signed certificate. The patched executables in [`cracked_exe/`](../cracked_exe) and the [`exe_patch/`](../exe_patch) patches remove that check.

## Capturing game traffic

BF2142's FESL login uses **SSLv3**, which current OpenSSL packages no longer support. `make ssl3` builds stcppipe with a private OpenSSL 1.1.1w that has SSLv3 and RC4 enabled:

```sh
cd src
make ssl3     # downloads and checks OpenSSL 1.1.1w, then builds stcppipe statically against it
./stcppipe -S -X ssl3 "" "" -d dumps <fesl_host> 18300 18300
```

Point the FESL host name at the machine running stcppipe (for example through the hosts file). The decrypted traffic is written to `dumps/` in pcap format and can be opened in Wireshark.

> [!WARNING]
> The private OpenSSL build is insecure on purpose. Use it only for this capture proxy.

`make` on its own builds stcppipe against the system OpenSSL (1.0.x, or 1.1 and later), without SSLv3. Windows binaries are available from the [author's site](http://aluigi.altervista.org/mytoolz.htm).

## FESL protocol overview

Every FESL packet has a 12-byte header followed by a text body:

| Bytes | Field |
| --- | --- |
| 0–3 | Command, ASCII: `fsys`, `acct`, `subs`, `dobj`, ... |
| 4–7 | Big endian: `type << 24 \| packet number`. An answer uses the number of its request |
| 8–11 | Big endian: total packet size, header included |
| 12– | `key=value\n` lines, terminated by a NUL byte. Every packet has a `TXN` key |

Packet types are `0x80` and `0xC0` for single packets, and `0xB0` and `0xF0` for the pieces of a multipart packet. A multipart packet carries its body base64-encoded in `data=` pieces, with the total size in `size=`.

A captured login runs in this order:

1. `fsys` Hello, then the server's MemCheck
2. `acct` Login
3. `subs` GetEntitlementByBundle
4. `dobj` GetObjectInventory
5. `acct` GetSubAccounts, LoginSubAccount and GetAccount
6. `acct` GameSpyPreAuth

See [`stuff/info/`](stuff/info) for the original notes.

## Contributing

Fork the repository and open a [pull request](https://github.com/blue-tj01/bf2142stats_emu/pulls). New captures, protocol notes and tests are as welcome as code. The [real-game test plan](docs/real-game-test-plan.md) describes how to record sessions with the real client and dedicated server. Please run `make -C tests test` before submitting.

## License

This folder is licensed under the [GNU GPL v2](../LICENSE), like the rest of the repository.

Bundled third-party code keeps its own license:

| Component | License |
| --- | --- |
| stcppipe | GPL v2 or later |
| minIni | [Apache 2.0](doc/apache-2.0.txt) |

> [!NOTE]
> Apache 2.0 code can't be combined with GPL v2-only code, so this has to be resolved before minIni is linked into the server.

## Credits

- [Artyom Shcherbakov (Tema567)](http://art567.ru): project author
- [Luigi Auriemma (aluigi)](http://aluigi.altervista.org): stcppipe and the FESL protocol research
- [CompuPhase](https://www.compuphase.com/minini.htm): minIni

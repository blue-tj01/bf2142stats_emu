rem WARNING: -restore_srv emails players their account name and password through
rem blat.exe, building the command line from the e-mail address stored for the
rem account. A crafted address may be able to run commands on this machine, and
rem passwords are sent in plain text. Only use this on a private server.
start fesl_login_server -v -l 1 -restore_srv -dbhost 127.0.0.1 -dbname test2142 -dbuser us2142 -dbpass pw2142

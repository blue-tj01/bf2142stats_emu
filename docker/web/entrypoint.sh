#!/bin/sh
set -e
php /usr/local/bin/bf2142-configure.php /var/www/html/include/_ccconfig.php
exec docker-php-entrypoint "$@"

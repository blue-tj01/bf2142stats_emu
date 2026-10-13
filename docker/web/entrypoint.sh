#!/bin/sh
set -e
CONFIG_DIR=/var/www/html/include
if [ ! -f "$CONFIG_DIR/_ccconfig.php" ]; then
    cp "$CONFIG_DIR/_ccconfig.example.php" "$CONFIG_DIR/_ccconfig.php"
    chown www-data:www-data "$CONFIG_DIR/_ccconfig.php"
fi
php /usr/local/bin/bf2142-configure.php /var/www/html/include/_ccconfig.php
exec docker-php-entrypoint "$@"

# Web stats server: Apache + PHP 8.3 serving web/ (including the .aspx endpoints)
FROM php:8.3-apache

RUN docker-php-ext-install mysqli \
    && cp "$PHP_INI_DIR/php.ini-production" "$PHP_INI_DIR/php.ini" \
    # web/.htaccess maps .aspx to PHP and blocks the log files, so it must apply
    && sed -i '/<Directory \/var\/www\/>/,/<\/Directory>/ s/AllowOverride None/AllowOverride All/' /etc/apache2/apache2.conf

COPY docker/web/php.ini "$PHP_INI_DIR/conf.d/zz-bf2142.ini"
COPY docker/web/entrypoint.sh /usr/local/bin/bf2142-entrypoint
COPY docker/web/configure.php /usr/local/bin/bf2142-configure.php
COPY web/ /var/www/html/

# The endpoints write request dumps and round logs into the web root, and
# error logs into ../logs (relative to it)
RUN chmod +x /usr/local/bin/bf2142-entrypoint \
    && mkdir -p /var/www/html/logs /var/www/logs \
    && chown -R www-data:www-data /var/www/html /var/www/logs

ENTRYPOINT ["bf2142-entrypoint"]
CMD ["apache2-foreground"]

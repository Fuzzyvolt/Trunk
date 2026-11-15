#!/bin/sh
# "/" redirects here. There is no index.html on purpose: with CGI on and basic
# auth off, BusyBox httpd returns 404 for "/" when index.html exists.
echo "Status: 302 Found"
echo "Location: /cgi-bin/status"
echo ""

#
# Regular cron jobs for the libnice package
#
0 4	* * *	root	[ -x /usr/bin/libnice_maintenance ] && /usr/bin/libnice_maintenance

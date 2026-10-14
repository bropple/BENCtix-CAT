# The BENCO logo on ssh logins only: at 76 columns it does not fit the
# phone's 60-column console. /etc/motd stays empty.
if [ -n "$SSH_CONNECTION" ] && [ -t 1 ] && [ -r /usr/share/benctix-cat/banner ]; then
	cat /usr/share/benctix-cat/banner
fi

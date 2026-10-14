// SPDX-License-Identifier: MIT
/*
 * libsystemd.so.0 for programs that call systemd and need nothing from it
 * (D050, D056): WebKitGTK sends its log lines to the journal, Samba tells
 * systemd it is ready and logs to the journal. This OpenRC system has
 * neither, so these take the calls and do nothing (sd_notify: "no notify
 * socket", as on a system without one).
 *
 * Installed in /usr/lib/s22-standin, outside the library path: only the
 * programs started with it on LD_LIBRARY_PATH (s22-browser, the file
 * sharing service) find it. Nothing else of systemd is here.
 */

__attribute__((visibility("default")))
int sd_journal_send(const char *format, ...)
{
	(void)format;
	return 0;
}

__attribute__((visibility("default")))
int sd_journal_send_with_location(const char *file, const char *line,
				  const char *func, const char *format, ...)
{
	(void)file;
	(void)line;
	(void)func;
	(void)format;
	return 0;
}

__attribute__((visibility("default")))
int sd_notify(int unset_environment, const char *state)
{
	(void)unset_environment;
	(void)state;
	return 0;
}

__attribute__((visibility("default")))
int sd_notifyf(int unset_environment, const char *format, ...)
{
	(void)unset_environment;
	(void)format;
	return 0;
}

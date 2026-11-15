/*
 * leddaemon - owns LED D3 (MIO47) and button PB1 (MIO51) on the MicroZed.
 *
 * - Pressing PB1 toggles the LED (interrupt-driven, debounced).
 * - Commands "on", "off" or "toggle" written to /tmp/led.fifo set the LED.
 *   Example: echo on > /tmp/led.fifo
 * - The current state ("0" or "1") is kept in /tmp/led.state for the web page.
 *
 * Uses the libgpiod v1 API (the version in this Buildroot image).
 *
 * Run in the foreground (logs to the terminal):  leddaemon -f
 * Normally started at boot by /etc/init.d/S99leddaemon (logs go to syslog,
 * read them with: logread | grep leddaemon  or  cat /var/log/messages).
 */
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define CHIP_NAME    "gpiochip0"
#define LED_LINE     47            /* MIO47 = LED D3 */
#define BTN_LINE     51            /* MIO51 = button PB1, 1 = pressed */
#define FIFO_PATH    "/tmp/led.fifo"
#define STATE_PATH   "/tmp/led.state"
#define CONSUMER     "leddaemon"
#define DEBOUNCE_MS  30            /* button must be quiet this long */

static volatile sig_atomic_t stop;
static struct gpiod_line *led;
static int led_on;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* Write the state to a temp file, then rename: readers never see a half-written file. */
static void write_state(void)
{
	FILE *f = fopen(STATE_PATH ".tmp", "w");

	if (!f) {
		syslog(LOG_WARNING, "cannot write %s: %m", STATE_PATH);
		return;
	}
	fprintf(f, "%d\n", led_on);
	fclose(f);
	rename(STATE_PATH ".tmp", STATE_PATH);
}

static void set_led(int on, const char *why)
{
	led_on = on ? 1 : 0;
	if (gpiod_line_set_value(led, led_on) < 0)
		syslog(LOG_ERR, "set LED: %m");
	write_state();
	syslog(LOG_INFO, "LED %s (%s)", led_on ? "on" : "off", why);
}

static void handle_command(const char *cmd)
{
	if (strcmp(cmd, "on") == 0)
		set_led(1, "command");
	else if (strcmp(cmd, "off") == 0)
		set_led(0, "command");
	else if (strcmp(cmd, "toggle") == 0)
		set_led(!led_on, "command");
	else
		syslog(LOG_WARNING, "unknown command '%s'", cmd);
}

int main(int argc, char **argv)
{
	int foreground = (argc > 1 && strcmp(argv[1], "-f") == 0);
	struct gpiod_chip *chip;
	struct gpiod_line *btn;
	struct sigaction sa;
	struct pollfd pfd[2];
	int fifo, btn_stable, pending = 0, ret = 1;

	openlog(CONSUMER, LOG_PID | (foreground ? LOG_PERROR : 0), LOG_DAEMON);

	/* No SA_RESTART: a signal interrupts poll() so the loop can exit cleanly. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	chip = gpiod_chip_open_by_name(CHIP_NAME);
	if (!chip) {
		syslog(LOG_ERR, "open %s: %m", CHIP_NAME);
		return 1;
	}
	led = gpiod_chip_get_line(chip, LED_LINE);
	btn = gpiod_chip_get_line(chip, BTN_LINE);
	if (!led || !btn) {
		syslog(LOG_ERR, "get lines: %m");
		goto out_chip;
	}
	if (gpiod_line_request_output(led, CONSUMER, 0) < 0) {
		syslog(LOG_ERR, "request LED line %d: %m (is another program using it?)", LED_LINE);
		goto out_chip;
	}
	/* Both edges: every change of the button wakes us up (an interrupt in the kernel). */
	if (gpiod_line_request_both_edges_events(btn, CONSUMER) < 0) {
		syslog(LOG_ERR, "request button line %d: %m", BTN_LINE);
		goto out_led;
	}
	btn_stable = gpiod_line_get_value(btn);

	/* The FIFO is opened read-write, so it never reports end-of-file
	 * when a writer (like "echo on > /tmp/led.fifo") closes it. */
	unlink(FIFO_PATH);
	if (mkfifo(FIFO_PATH, 0620) < 0) {
		syslog(LOG_ERR, "mkfifo %s: %m", FIFO_PATH);
		goto out_btn;
	}
	fifo = open(FIFO_PATH, O_RDWR | O_NONBLOCK);
	if (fifo < 0) {
		syslog(LOG_ERR, "open %s: %m", FIFO_PATH);
		goto out_unlink;
	}

	set_led(0, "startup");
	syslog(LOG_INFO, "running: LED MIO%d, button MIO%d, commands via %s",
	       LED_LINE, BTN_LINE, FIFO_PATH);

	pfd[0].fd = gpiod_line_event_get_fd(btn);
	pfd[0].events = POLLIN;
	pfd[1].fd = fifo;
	pfd[1].events = POLLIN;

	while (!stop) {
		/* Sleep until the button changes or a command arrives.
		 * While the button is bouncing, wake up after DEBOUNCE_MS of quiet. */
		int n = poll(pfd, 2, pending ? DEBOUNCE_MS : -1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "poll: %m");
			break;
		}

		if (n == 0 && pending) {
			/* Button has been quiet long enough: read its settled value. */
			int v = gpiod_line_get_value(btn);

			pending = 0;
			if (v >= 0 && v != btn_stable) {
				btn_stable = v;
				if (v == 1)
					set_led(!led_on, "button");
			}
		}

		if (pfd[0].revents & POLLIN) {
			struct gpiod_line_event ev;

			if (gpiod_line_event_read(btn, &ev) == 0)
				pending = 1;    /* start (or restart) the debounce wait */
		}

		if (pfd[1].revents & POLLIN) {
			char buf[128];
			ssize_t len = read(fifo, buf, sizeof(buf) - 1);

			if (len > 0) {
				char *tok;

				buf[len] = '\0';
				for (tok = strtok(buf, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n"))
					handle_command(tok);
			}
		}
	}

	syslog(LOG_INFO, "stopping");
	gpiod_line_set_value(led, 0);
	ret = 0;
	close(fifo);
	unlink(STATE_PATH);
out_unlink:
	unlink(FIFO_PATH);
out_btn:
	gpiod_line_release(btn);
out_led:
	gpiod_line_release(led);
out_chip:
	gpiod_chip_close(chip);
	closelog();
	return ret;
}

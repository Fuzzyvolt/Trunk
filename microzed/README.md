# MicroZed files

Source for the programs and the Buildroot rootfs overlay used in `buildroot-microzed-project.md`.

## Layout

| Path | What it is |
|---|---|
| `apps/hello.c` | Cross-compile test |
| `apps/blink.c` | Blinks LED D3 until PB1 is pressed (libgpiod v1) |
| `apps/leddaemon.c` | Daemon that owns LED D3 and PB1; takes commands from `/tmp/led.fifo` |
| `overlay/` | Files copied into the board's root filesystem (mirrors `/`) |
| `overlay/etc/init.d/S90httpd` | Starts BusyBox httpd at boot |
| `overlay/etc/init.d/S99leddaemon` | Starts leddaemon at boot |
| `overlay/etc/network/interfaces` | eth0 DHCP with the fixed MAC `02:0a:35:00:00:01` |
| `overlay/srv/www/cgi-bin/` | Web page: `status`, `control` (buttons), `index.cgi` (redirect for `/`) |
| `configs/busybox.fragment` | BusyBox options (httpd + CGI) |
| `Makefile` | Build, copy to the overlay, or push to the board |

Not in git: the SSH host keys in `~/mz-overlay/etc/dropbear/` (private).

## Get the files on the Mint laptop

First time:

```bash
git clone -b claude/micro-zed-project-ideas-o0fk5t https://github.com/fuzzyvolt/trunk.git ~/trunk
```

After that, to get updates:

```bash
cd ~/trunk && git pull
```

## Use

```bash
cd ~/trunk/microzed
make            # build the programs
make push       # test on the running board right away (no re-flash)
make overlay    # copy into ~/mz-overlay for the next image
```

`make push` asks for the root password several times unless you set up a key with `ssh-copy-id root@10.0.0.134`.

## LED daemon

- Started at boot by `S99leddaemon`. Start/stop by hand: `/etc/init.d/S99leddaemon start|stop|restart`.
- Commands: `echo on > /tmp/led.fifo` (also `off`, `toggle`).
- PB1 toggles the LED.
- State for the web page: `/tmp/led.state`.
- Logs: `logread | grep leddaemon` (or `/var/log/messages`).
- Debug in the foreground: stop the service, then run `leddaemon -f`.
- `blink` and `gpioset` on lines 47/51 fail with "Device or resource busy" while the daemon runs. Stop it first.

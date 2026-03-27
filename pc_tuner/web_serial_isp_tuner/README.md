# web_serial_isp_tuner

Browser-based ISP tuning page using the Web Serial API.

## What it can do

- open a Windows COM port directly from the browser
- send `GET` / `SET` commands to `t23_isp_uartd`
- display current parameter values
- request JPEG snapshots using `SNAP`
- poll snapshots in a simple auto-preview loop

## Recommended browser

- Microsoft Edge
- Google Chrome

## How to serve it

Use a local static server so the page runs from `http://localhost`.

Example from the repository root in WSL:

```sh
cd ~/T23-C3-Project/pc_tuner/web_serial_isp_tuner
python3 -m http.server 8080
```

Then open on Windows:

- `http://localhost:8080`

## Expected T23 command protocol

Examples:

- `PING`
- `GET ALL`
- `GET BRIGHTNESS`
- `SET BRIGHTNESS 140`
- `SNAP`

Snapshot responses:

1. text header: `JPEG <length>`
2. raw JPEG payload of that exact length

## Recommended first bring-up workflow

1. On the T23 board, start the tuning daemon on the UART console port:

```sh
/system/init/start_isp_uartd.sh /dev/ttyS1 921600
```

2. Close the terminal program that was holding the COM port.
3. In Edge or Chrome, open `http://localhost:8080`.
4. Click `Connect Serial` and choose the matching Windows COM port, usually
   `COM3`.
5. Click `Refresh Values`, then test one slider and finally `Capture Snapshot`.

## If the page only shows outgoing commands

If the log panel shows only lines like:

- `> GET ALL`
- `> SET BRIGHTNESS 120`
- `> SNAP`

but never shows any incoming lines like:

- `< VAL ...`
- `< OK ...`
- `< JPEG ...`

then the most likely cause is that `t23_isp_uartd` was started from the same
serial login session and died when that terminal window closed.

Use the updated startup helper:

```sh
/system/init/start_isp_uartd.sh /dev/ttyS1 921600
```

It now starts the daemon in the background and writes logs to:

- `/tmp/isp_uartd.log`
- `/tmp/isp_uartd.pid`

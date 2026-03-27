# pc_tuner

PC-side tuning tools for the T23 rebuild project.

## Current tool

- `web_serial_isp_tuner/`

This is a browser-based ISP tuning UI that uses the Web Serial API.

## Why Web Serial

For the current stage it is the lightest path:

- no need to finish C3 networking first
- no need to reverse the official ImageTool socket protocol
- no need to install a desktop GUI framework first
- the browser can talk directly to a Windows COM port

## Recommended usage

1. run a local static file server for `web_serial_isp_tuner`
2. open the page in Edge or Chrome
3. click `Connect Serial`
4. select the T23 UART COM port, currently recommended to be the UART log port (`COM3`)
5. use sliders and `Refresh Values`
6. use `Capture Snapshot` or `Auto Preview` to observe tuning changes

## Important note

Web Serial works best from a trustworthy origin such as `http://localhost`.
For that reason, serve the folder instead of opening `index.html` directly from
disk.

## Port recommendation

- Preferred MVP port: `COM3`
  because it is already the visible UART log/debug line
- Not recommended for MVP: `COM8`
  because it is more likely part of the T23 flashing/programming path and may
  require extra USB-device support before it behaves like a normal runtime
  serial port

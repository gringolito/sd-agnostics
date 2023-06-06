# SD Card Diagnostics (sd-agnostics)

SD diagnostics tool for testing Application Class SD Card performance

## Installation

TODO

## Usage

```console
$ sd-agnostics -h
Usage: sd-agnostics [-d PATH] [-h]

SD diagnostics tool for testing Application Class SD Card performance

Options:
  -d PATH   Set the mountpoint to run the tests (Default: /var/tmp)
  -h        Print this help message
```

Example:

```console
$ sd-agnostics
Run 1
prepare-file;0;0;27757;54
seq-write;0;0;27982;54
rand-4k-write;0;0;2204;551
rand-4k-read;9564;2391;0;0
Sequential write speed 27982 KB/sec (target 10000) - PASS
Random write speed 551 IOPS (target 500) - PASS
Random read speed 2391 IOPS (target 1500) - PASS
```

## Future work

- Create multiple application profiles (support for A2 application class)

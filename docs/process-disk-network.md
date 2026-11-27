# Disk and Network columns

The Processes page has two columns modelled on the Windows Task Manager: **Disk** and **Network**.
Both are shown by default, can be hidden from the header's context menu, and are only measured while
they (or the I/O columns, for Disk) are visible.

## Disk

Read plus write rate of the process, taken from `write_bytes` and `read_bytes` of `/proc/PID/io`
(the same data as the *IO Read/s* and *IO Write/s* columns, added together).

* Counts bytes that reach the block layer. Reads served from the page cache and writes to tmpfs do not count.
* Another user's processes need root (`?` is shown), like the I/O columns.
* `measuring...` is shown until two samples exist.

## Network

Throughput of the process in bits per second, **TCP only**.

### How it works

1. The byte counters of every TCP socket are read through netlink `sock_diag` (the interface behind
   `ss -ti`): `tcpi_bytes_acked + tcpi_bytes_received` from the kernel's `struct tcp_info`.
2. Each socket is attributed to the process holding its inode in `/proc/PID/fd` (`socket:[inode]`).
3. The value is the bytes moved by the process's sockets since the previous refresh divided by the elapsed time.

It needs no root and no library. File descriptors are cached per process and re-read every 5 refreshes
(staggered by PID), so a process that opens its first connection can take up to 5 refreshes to be attributed.

### What it cannot see

The kernel does not expose per-process network counters, so this is a **lower bound** of the real traffic:

| Not counted | Why |
|---|---|
| UDP (QUIC/HTTP3, DNS, games, VoIP) | The kernel keeps no byte counters for UDP sockets |
| Bytes of sockets that close between two refreshes | Their counters disappear with the socket |
| Traffic of other users' processes | Their `/proc/PID/fd` needs root (`?` is shown) |
| Other network namespaces (containers) | `sock_diag` is queried in our own namespace |

Loopback traffic **is** counted. A socket shared by several processes (after `fork`) is attributed to one of them.

Exact per-process accounting for all protocols needs privileges (packet capture with `CAP_NET_RAW`, eBPF,
conntrack accounting) and is not implemented.

### What the cell shows

| Cell | Meaning |
|---|---|
| `23.6 Mb/s`, `0 b/s` | Measured |
| `—` | Not measurable yet (first sample), or `sock_diag` is unavailable on this system |
| `?` | Another user's process that cannot be inspected without root |

If `sock_diag` cannot be read, a warning is logged once and the column shows `—`. Kernels older than 4.1
do not report the byte counters in `tcp_info` and the column shows `—` as well.

## Verification

Measured on Fedora 44, kernel 7.3, against known loads:

| Load | Expected | Shown |
|---|---|---|
| `curl --limit-rate 1M` over loopback | about 8 Mb/s | 7.4 to 9.2 Mb/s, same value for client and server |
| `curl --limit-rate 3M` over loopback | about 24 Mb/s | 22.8 Mb/s (curl) and 23.6 Mb/s (server) |
| `dd oflag=direct` to a local disk | whatever the disk does | 130.7 MB/s in Disk, top of the list |

Not verified: IPv6-only traffic, the tree view, other users' processes, and kernels other than the one above.
The `struct tcp_info` offsets used (120 and 128) were checked against the kernel headers of that machine.

To cross-check on your system, compare with `ss -ti` (the `bytes_acked` and `bytes_received` fields of a socket)
or with `nethogs` (which also sees UDP, but needs root).

## Unit tests

```sh
cd tests/processnet
qmake6 processnet.pro && make && ./processnet_test
```

They cover the `sock_diag` parser (including truncated, empty and error replies), the `tcp_info` layout,
the `socket:[inode]` parser and the delta math, and run in CI.

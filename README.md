# ProxyC
![C](https://img.shields.io/badge/C-A8B9CC?style=for-the-badge&logo=c&logoColor=white)
![SQLite](https://img.shields.io/badge/SQLite-003B57?style=for-the-badge&logo=sqlite&logoColor=white)
![Proxy](https://img.shields.io/badge/Proxy-4B5563?style=for-the-badge&logo=cloudflare&logoColor=white)
![CLI](https://img.shields.io/badge/CLI-000000?style=for-the-badge&logo=gnubash&logoColor=white)

> rewrite of [this repo](https://github.com/PeacexF/Proxy-Strainer) in C

**ProxyC** is a high-performance, asynchronous proxy checker and manager written in pure *C*. Uses *sqlite* as a unified storage.

By transitioning from synchronous thread-per-proxy pooling to non-blocking I/O, ProxyC can scale to handle thousands of concurrent proxy validations using minimal memory and near-zero CPU overhead.


## Status 
*Finished*  
Maybe will add some features in the future  
This project was orginally made to improve the performance of my already existing script, as well as to learn C.  

## Features

* **Asynchronous Networking:** Uses `libcurl`'s multi-interface to check hundreds of proxies concurrently in a single event loop thread, avoiding thread context switching bottlenecks.

* **Unified SQLite Backend:** A single database table stores everything.

* **SQLite Performance:** Tuned with pragmas to match RAM-disk write speeds.

* **Bulk Import:** Imports massive proxy lists inside SQL transactions.

## Project Structure

```text
└── strainer-c/
    ├── Makefile
    ├── data/
    │   └── proxies.txt     your initial proxy list, .db is also here
    ├── include/
    │   ├── checker.h
    │   ├── db.h            <- header files
    │   └── parser.h
    ├── list.txt            list of repos to take proxies from
    ├── src/
    │   ├── checker.c       proxt checker
    │   ├── db.c            sqlite
    │   ├── main.c          cli
    │   ├── parser.c        parsing
    └── strainer            binary, created after `make` cmd
```

## Prerequisites

Ensure you have the necessary development tools and libraries installed on your Unix-like operating system (Linux / macOS):

* `gcc` or `clang`
* `make`
* `libcurl`
* `sqlite3`

### Installing Dependencies

**On Ubuntu/Debian:**

```bash
sudo apt update
sudo apt install build-essential libcurl4-openssl-dev libsqlite3-dev
```

**On macOS (via Homebrew):**

```bash
brew install curl sqlite
```

## Building the Project

Compiling the project is automated using the `Makefile`:

```bash
make
```

This generates the standalone binary executable named `strainer`.

To clean up compiled object files and binaries:

```bash
make clean
```

## CLI Usage

``` text
Usage:
  ./strainer --import  <file>                Import proxies
  ./strainer --check                         Check unchecked proxies
  ./strainer --retry                         Retry failed proxies
  ./strainer --export  <file> [--scheme X]   Export active proxies
  ./strainer --stats                         Show statistics

Options:
  --db      <path>   SQLite DB path     (default: data/proxies.db)
  --threads <n>      Concurrent conns   (default: 512)
  --timeout <n>      Timeout seconds    (default: 10)
  --url     <url>    Test URL           (default: http://httpbin.org/get)
  --scheme  <s>      Scheme filter for --export (e.g. socks5://)
```
## Database 

```sql
CREATE TABLE IF NOT EXISTS proxies (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    address      TEXT UNIQUE NOT NULL,       
    status       TEXT NOT NULL DEFAULT 'unchecked', 
    error_type   TEXT DEFAULT NULL,         
    latency_ms   INTEGER DEFAULT NULL,       
    last_checked TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_status ON proxies(status);
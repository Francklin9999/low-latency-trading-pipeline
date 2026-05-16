# Data

This project replays a real NASDAQ TotalView-ITCH 5.0 trading day. The replay
file and the protocol specification are not redistributed in this repository,
but both are published publicly by NASDAQ. Download them once and drop them in
this directory.

## Files this directory expects

| File | Size | What |
|---|---:|---|
| `01302020.NASDAQ_ITCH50` | ~12.9 GB (uncompressed) | One full trading day (2020-01-30) of NASDAQ TotalView-ITCH 5.0 messages. Drives the entire replay. |
| `NQTVITCHSpecification.pdf` | ~1.2 MB | NASDAQ TotalView-ITCH 5.0 protocol spec -- message types, wire format, sequencing rules. Read this if you touch `src/itch/`. |

## Where to download

NASDAQ publishes historical ITCH sample files via their EMI server. The file
this project tests against:

```bash
# fetch the gzipped sample, decompress, and place under data/
curl -O https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302020.NASDAQ_ITCH50.gz
gunzip 01302020.NASDAQ_ITCH50.gz
mv 01302020.NASDAQ_ITCH50 /path/to/repo/data/
```

The directory listing of available days lives at
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/` -- any file from there will work
as a drop-in; the only date-coupled thing in the engine is
`include/hft/engine/universe.hpp`, which is tuned for the symbol locates that
appeared in `01302020`. To use a different day, regenerate that header from
`scripts/utils/top_symbols.py`.

The protocol specification PDF:

```bash
curl -O https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf
mv NQTVITCHSpecification.pdf /path/to/repo/data/
```

If either URL has moved (NASDAQ does reshuffle their site occasionally), search
for "NASDAQ TotalView-ITCH 5.0 specification" and "NASDAQ ITCH sample file"
-- both are still freely published, just sometimes under different paths.

## License / redistribution

NASDAQ publishes these as historical samples for technical evaluation. They are
not redistributable through this repository, which is why the `.gitignore` is
set up to exclude `data/*ITCH*`. Re-downloading from the upstream source above
is the supported path.

## Verifying the file

The exact file used in [Results](../README.md#results) should hash to:

```bash
sha256sum data/01302020.NASDAQ_ITCH50
# 12,952,050,754 bytes
```

(Record the SHA256 once you've downloaded it so you can spot a partial transfer
or a re-published variant from upstream.)

#pragma once

// Device identity — change per unit if you deploy more than one.
#define DEVICE_ID        "barn_minisplit"
#define FRIENDLY_NAME    "Barn Mini-Split"
#define MANUFACTURER     "Mitsubishi"
#define MODEL            "MSZ-FH15NA"    // Mitsubishi FH-series 15kBTU wall-mount, confirmed 2026-10-12

// HTTP server port and mDNS hostname. CozyHem polls GET /status and POSTs to
// /command; the mDNS record lets the MCP server and CozyHem adapter address
// the Pico as "barn-minisplit.local" without a static-IP reservation.
#define HTTP_PORT        80
#define MDNS_HOSTNAME    "barn-minisplit"

// CN105 data pins. These are PIO software-serial pins, NOT hardware UART1:
// the working orientation is TX=GP5 / RX=GP4, and UART1 permits TX only on
// {4,8,20,24}, so GP5 cannot be a hardware TX. Wiring is straight through
// (CN105 pin 4 -> GP4, pin 5 -> GP5); the roles below are what the unit needs.
#define CN105_TX_PIN     5        // GP5 -> shifter -> CN105 pin 5 = unit's RX input
#define CN105_RX_PIN     4        // GP4 <- shifter <- CN105 pin 4 = unit's TX output
#define CN105_BAUD       2400     // Unused: HeatPump::connect() opens the port itself at 2400 8E1 (EVEN parity)

// How often to poll the heatpump for state (ms). NOTE: hp.sync() is NOT
// non-blocking -- when the CN105 link is down it calls connect(), which costs
// about 6 s (two delay(2000) calls plus two ~1 s waits) and starves
// server.handleClient(). loop() therefore only syncs while isConnected() and
// rate-limits reconnect attempts; see CN105_RETRY_MS in main.cpp.
#define SYNC_INTERVAL_MS 1000

// WiFi reconnect backoff (ms) if the link drops.
#define WIFI_RETRY_MS    10000

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

// UART1 pins on the Pico W for CN105. Change if you wire differently.
#define CN105_TX_PIN     4
#define CN105_RX_PIN     5
#define CN105_BAUD       2400     // Mitsubishi protocol is fixed at 2400 8N1

// How often to poll the heatpump for state (ms). SwiCago's hp.sync() is
// non-blocking; this just sets the main loop cadence.
#define SYNC_INTERVAL_MS 1000

// WiFi reconnect backoff (ms) if the link drops.
#define WIFI_RETRY_MS    10000

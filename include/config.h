#pragma once

// Device identity — change per unit if you deploy more than one.
#define DEVICE_ID        "barn_minisplit"
#define FRIENDLY_NAME    "Barn Mini-Split"
#define MANUFACTURER     "Mitsubishi"
#define MODEL            "unknown"       // fill in from your unit's model sticker

// MQTT topic structure.
// State and command topics follow the Home Assistant MQTT climate pattern so
// discovery payloads below can advertise the right endpoints.
#define TOPIC_BASE       "cozyhem/climate/" DEVICE_ID
#define TOPIC_STATE      TOPIC_BASE "/state"
#define TOPIC_CMD        TOPIC_BASE "/cmd"
#define TOPIC_AVAIL      TOPIC_BASE "/available"

// Home Assistant MQTT discovery prefix. CozyHem's auto-discovery listens here.
#define HA_DISCOVERY_PREFIX "homeassistant"

// UART1 pins on the Pico W for CN105. Change if you wire differently.
#define CN105_TX_PIN     4
#define CN105_RX_PIN     5
#define CN105_BAUD       2400     // Mitsubishi protocol is fixed at 2400 8N1

// How often to poll the heatpump for state (ms). SwiCago's hp.sync() is
// non-blocking; this just sets the main loop cadence.
#define SYNC_INTERVAL_MS 1000

// WiFi reconnect backoff (ms) if the link drops.
#define WIFI_RETRY_MS    10000

#pragma once
#include <stdbool.h>

// Initialize Wi-Fi in station mode using credentials from wifi_config.h
// Returns true if startup/init succeeded (does not guarantee connection unless WIFI_BLOCK_ON_START=1)
bool wifi_init_sta(void);

// Optionally block until connected (returns true if connected)
bool wifi_wait_connected(int timeout_ms);

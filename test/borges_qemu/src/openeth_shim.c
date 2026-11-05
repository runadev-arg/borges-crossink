// The SDK's prebuilt libs have CONFIG_ETH_ENABLED but not CONFIG_ETH_USE_OPENETH, and
// turning it on in custom_sdkconfig would change the libs under test (and make pioarduino
// rebuild the shared framework). So the sources that option would add are compiled here,
// against the same headers. Buffer counts are IDF's Kconfig defaults.
#include "sdkconfig.h"
#define CONFIG_ETH_USE_OPENETH 1
#define CONFIG_ETH_OPENETH_DMA_RX_BUFFER_NUM 4
#define CONFIG_ETH_OPENETH_DMA_TX_BUFFER_NUM 1
#include "esp_eth_mac_openeth.c"

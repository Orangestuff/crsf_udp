# 1. Register your main application and its required dependencies
idf_component_register(SRCS "esp32Eth.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_netif esp_event lwip cherry-embedded__cherryusb)

# --- THE CMAKE SCALPEL ---
set(CHERRY_DIR "${CMAKE_CURRENT_LIST_DIR}/../managed_components/cherry-embedded__cherryusb")

# 2. Point EXACTLY to the core network files, ignoring the NuttX OS ports
target_sources(${COMPONENT_LIB} PRIVATE 
    "${CHERRY_DIR}/class/net/usbh_net.c"
    "${CHERRY_DIR}/class/net/usbh_rtl8152.c"
)

# 3. Force the preprocessor to activate the driver code natively
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_USBHOST_RTL8152=1 CONFIG_USBHOST_NETWORK=1)
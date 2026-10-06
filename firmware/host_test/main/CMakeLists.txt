idf_component_register(
    SRCS         "host_test_main.c"
    INCLUDE_DIRS "."
    REQUIRES     unity access_core ui keypad lock_driver display_ssd1306 gate_ctrl persist_store wifi_prov net_link tg_client code_sync
    WHOLE_ARCHIVE
)

# Shared test files, compiled into this app so host runs and device runs
# execute exactly the same assertions.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/access_core/test/test_access_core.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/ui/test/test_ui.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/keypad/test/test_keypad_logic.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/lock_driver/test/test_lock_logic.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/display_ssd1306/test/test_oled.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/gate_ctrl/test/test_gate_ctrl.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/persist_store/test/test_persist_ring.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/persist_store/test/test_codes.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_prov/test/test_prov_logic.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/wifi_prov/test/test_prov_form.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/net_link/test/test_net_retry.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/tg_client/test/test_tg_proto.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/code_sync/test/test_sync_logic.c"
)
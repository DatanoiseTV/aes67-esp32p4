idf_component_register(
    SRCS
        "src/aes67.c"
        "src/aes67_ptp.c"
        "src/aes67_rtp.c"
        "src/aes67_sap.c"
        "src/aes67_sdp.c"
        "src/aes67_session.c"
        "src/aes67_audio.c"
        "src/aes67_convert.c"
        "src/aes67_net.c"
        "src/aes67_mdns.c"
        "src/aes67_hw_timer.c"
        "src/aes67_l2tap_rx.c"
        "src/aes67_webui.c"
        # Bundled third-party: a minimal IEEE-1588 PTP daemon (Apache NuttX
        # ptpd, ported to ESP-IDF by Espressif) and the EMAC time wrapper from
        # the ESP-IDF ethernet/ptp example. Vendored here so the component is
        # self-contained on the Component Registry. See third_party/*/README.md
        # and the SPDX headers for provenance and licensing.
        "third_party/ptpd/ptpd.c"
        "third_party/esp_eth_time/esp_eth_time.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
        "third_party/ptpd"
        "third_party/ptpd/include"
        "third_party/esp_eth_time"
    PRIV_REQUIRES
        esp_eth
        esp_netif
        esp_timer
        esp_driver_i2s
        mdns
        lwip
        esp_psram
        esp_mm
        esp_http_server
)

# ptpd.c is a third-party port with intentionally-unused helpers behind
# Kconfig; silence that one warning for the bundled file only.
set_source_files_properties(
    third_party/ptpd/ptpd.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function"
)

# Gzip the web frontend at build time, into the build directory, and embed it.
# Python's gzip with mtime=0 makes the output reproducible (no timestamp or
# file name in the header), so nothing generated lands in the source tree.
idf_build_get_property(python PYTHON)
set(AES67_WEB_GZ ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)
add_custom_command(
    OUTPUT ${AES67_WEB_GZ}
    COMMAND ${python} -c "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))"
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${AES67_WEB_GZ}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
    COMMENT "Compressing web/index.html -> index.html.gz"
    VERBATIM
)
add_custom_target(aes67_web_gz DEPENDS ${AES67_WEB_GZ})
# symbol names follow the file name: _binary_index_html_gz_start/_end
target_add_binary_data(${COMPONENT_LIB} ${AES67_WEB_GZ} BINARY DEPENDS aes67_web_gz)

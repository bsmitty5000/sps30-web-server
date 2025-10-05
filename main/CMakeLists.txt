idf_component_register(
    SRCS 
        "main.c"
    PRIV_REQUIRES 
        spi_flash
    INCLUDE_DIRS 
        ""
    REQUIRES
        sps30
)

set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../www")

if(EXISTS ${WEB_SRC_DIR})
    set(WEB_BUILD_DIR "${CMAKE_BINARY_DIR}/web_build")
    
    add_custom_command(
        OUTPUT ${WEB_BUILD_DIR}/.copied
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WEB_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${WEB_SRC_DIR} ${WEB_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E touch ${WEB_BUILD_DIR}/.copied
        DEPENDS ${WEB_SRC_DIR}/index.html ${WEB_SRC_DIR}/style.css ${WEB_SRC_DIR}/app.js ${WEB_SRC_DIR}/favicon.ico
        COMMENT "Copying web assets to ${WEB_BUILD_DIR}"
    )

    add_custom_command(
        OUTPUT ${WEB_BUILD_DIR}/.compressed
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../compress_assets.py ${WEB_BUILD_DIR}
        COMMAND ${CMAKE_COMMAND} -E touch ${WEB_BUILD_DIR}/.compressed
        DEPENDS ${WEB_BUILD_DIR}/.copied
        COMMENT "Gzipping web assets"
    )

    add_custom_target(web_assets ALL DEPENDS ${WEB_BUILD_DIR}/.compressed)

    spiffs_create_partition_image(www ${WEB_BUILD_DIR} FLASH_IN_PROJECT DEPENDS web_assets)

else()
    message(FATAL_ERROR "${WEB_SRC_DIR}/dist doesn't exit. Please run 'npm run build' in ${WEB_SRC_DIR}")
endif()

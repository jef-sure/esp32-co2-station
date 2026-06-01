set(app_sources
    app_fonts.c
    app_mqtt.c
    app_settings.c
    app_webserver.c
    main.c
    provisioning.c
    str.c
    tzones.c
)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES            dgx esp_event esp_wifi nvs_flash protocomm wifi_provisioning esp_event mbedtls esp_http_client esp_http_server esp_adc esp_driver_i2c scd4x mqtt
    INCLUDE_DIRS "."
)

# if(NOT CMAKE_BUILD_EARLY_EXPANSION)
#     target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUG_APP_HEAP)
# endif()

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(APP_WEBSERVER_TEMPLATE_HTML ${CMAKE_CURRENT_LIST_DIR}/../misc/index-bs.html)
    set(APP_WEBSERVER_TEMPLATE_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/../misc/split_to_template.py)
    set(APP_WEBSERVER_TEMPLATE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/app_webserver_template.h)
    set(TZONES_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/../misc/generate_tzones.py)
    set(TZONES_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/tzones_data.inc)

    if(DEFINED PYTHON AND NOT "${PYTHON}" STREQUAL "")
        set(APP_TEMPLATE_PYTHON ${PYTHON})
    else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(APP_TEMPLATE_PYTHON ${Python3_EXECUTABLE})
    endif()

    add_custom_command(
        OUTPUT ${APP_WEBSERVER_TEMPLATE_HEADER}
        COMMAND ${APP_TEMPLATE_PYTHON} ${APP_WEBSERVER_TEMPLATE_SCRIPT} ${APP_WEBSERVER_TEMPLATE_HTML} ${APP_WEBSERVER_TEMPLATE_HEADER}
        DEPENDS ${APP_WEBSERVER_TEMPLATE_HTML} ${APP_WEBSERVER_TEMPLATE_SCRIPT}
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${TZONES_INCLUDE}
        COMMAND ${APP_TEMPLATE_PYTHON} ${TZONES_SCRIPT} build ${TZONES_INCLUDE}
        DEPENDS ${TZONES_SCRIPT}
        VERBATIM
    )

    add_custom_target(app_webserver_template DEPENDS ${APP_WEBSERVER_TEMPLATE_HEADER})
    add_custom_target(tzones_include DEPENDS ${TZONES_INCLUDE})
    add_dependencies(${COMPONENT_LIB} app_webserver_template)
    add_dependencies(${COMPONENT_LIB} tzones_include)
    set_source_files_properties(app_webserver.c PROPERTIES OBJECT_DEPENDS ${APP_WEBSERVER_TEMPLATE_HEADER})
    set_source_files_properties(tzones.c PROPERTIES OBJECT_DEPENDS ${TZONES_INCLUDE})
    target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

cmake_minimum_required(VERSION 3.16)

# Layered defaults: committed project defaults, then optional local credentials
# (sdkconfig.secrets is git-ignored; see sdkconfig.secrets.example).
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.secrets")
    else()
        message(WARNING "sdkconfig.secrets not found: Wi-Fi will use placeholder credentials. "
                        "Copy sdkconfig.secrets.example to sdkconfig.secrets and edit it.")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wroomcam)

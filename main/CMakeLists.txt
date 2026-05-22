file(GLOB_RECURSE MY_SRCS
    "./hal/*.c"
    "./hal/*.cc"
    "./hal/*.cpp"
    "./system/*.c"
    "./system/*.cc"
    "./system/*.cpp"
    "./apps/*.c"
    "./apps/*.cc"
    "./apps/*.cpp"
)
list(REMOVE_ITEM MY_SRCS "${CMAKE_CURRENT_LIST_DIR}/hal/external_input.cpp")
list(REMOVE_ITEM MY_SRCS "${CMAKE_CURRENT_LIST_DIR}/apps/app_external_input_test/app_external_input_test.cpp")

set(MY_INCLUDE_DIRS 
    "."
    "./assets"
    "./hal"
    "./apps"
)

set(PIKA_PYTHON_INCS
    ./apps/app_repl/PikaScript/pikascript-api/
    ./apps/app_repl/PikaScript/pikascript-core/
)

file(GLOB EXTERN_FILES assets/*)

idf_component_register(SRCS "main.cpp" "hal/external_input.cpp" "apps/app_external_input_test/app_external_input_test.cpp" ${MY_SRCS}
                    INCLUDE_DIRS ${MY_INCLUDE_DIRS} ${PIKA_PYTHON_INCS}
                    EMBED_FILES ${EXTERN_FILES}
)

cmake_minimum_required(VERSION 3.16)
project(Bedlam2PS5Frontend LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(BEDLAM2_PS5_TRACE "Log native startup milestones to the PS5 kernel log" OFF)
find_package(SDL2 CONFIG REQUIRED)

if(PS5)
    # The Support Kit overlay supplies controller text input. Native-game
    # startup must not use SDL's optional Sony USB keyboard/IME modules.
    get_target_property(_bedlam2_sdl_imports SDL2::SDL2 INTERFACE_LINK_LIBRARIES)
    if(_bedlam2_sdl_imports MATCHES "SceKeyboard|SceImeDialog")
        message(FATAL_ERROR
            "SDL2 requires the unavailable system keyboard/IME modules. "
            "Build the Support Kit patched SDL with SDL_PS5_KEYBOARD=OFF "
            "and set SDL2_DIR to that native installation's lib/cmake/SDL2.")
    endif()
endif()

find_program(BEDLAM2_JWASM_EXECUTABLE NAMES jwasm)

set(PS5PORT_SUPPORT_KIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ps5-port-support-kit"
    CACHE PATH "Path to the local PS5 Port Support Kit checkout")
if(NOT EXISTS "${PS5PORT_SUPPORT_KIT_DIR}/include/ps5port/overlay.hpp")
    message(FATAL_ERROR "PS5 Port Support Kit not found. Set PS5PORT_SUPPORT_KIT_DIR.")
endif()

file(GLOB BEDLAM2_CPP CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../CPP_Sources/*.cpp")
add_library(bedlam2_frontend OBJECT ${BEDLAM2_CPP})
target_sources(bedlam2_frontend PRIVATE
    native_engine_entry.S
    native_external_thunks.S
    native_engine_helpers.cpp
    native_engine_memory.cpp
    native_file_bridge.cpp
    native_pthread_bridge.cpp
    native_sdl_memory.cpp
    native_input.cpp)
target_include_directories(bedlam2_frontend PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../CPP_Sources"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ASM_Sources"
    "${PS5PORT_SUPPORT_KIT_DIR}/include"
    "${PS5PORT_SUPPORT_KIT_DIR}/vendor")
target_compile_definitions(bedlam2_frontend PRIVATE
    BEDLAM_USE_SDL_AUDIO_MIXER=1
    BEDLAM2_PS5_NATIVE=1)
target_compile_options(bedlam2_frontend PRIVATE
    -fno-exceptions
    -fno-rtti)
if(BEDLAM2_PS5_TRACE)
    target_compile_definitions(bedlam2_frontend PRIVATE BEDLAM2_PS5_TRACE=1)
endif()
target_link_libraries(bedlam2_frontend PRIVATE SDL2::SDL2)

# The frontend object target can be built on its own. When JWasm is available,
# the optional native target also links the converted engine into a PS5 ELF.

# Optional assembly migration and native ELF targets. The generated object still
# needs runtime validation for its legacy pointer, stack, and callback behavior.
if(BEDLAM2_JWASM_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(BEDLAM2_X64_ENGINE_DIR "${CMAKE_CURRENT_BINARY_DIR}/bedlam2-engine-x64")
    set(BEDLAM2_X64_ENGINE_OBJECT "${BEDLAM2_X64_ENGINE_DIR}/bedlam2_x64.o")
    add_custom_command(
        OUTPUT "${BEDLAM2_X64_ENGINE_OBJECT}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/asm64_prepare.py"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}/../ASM_Sources"
            --output-dir "${BEDLAM2_X64_ENGINE_DIR}"
            --jwasm "${BEDLAM2_JWASM_EXECUTABLE}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/asm64_prepare.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../ASM_Sources/bedlam2.asm"
            "${CMAKE_CURRENT_SOURCE_DIR}/../ASM_Sources/bedlam2.inc"
            "${CMAKE_CURRENT_SOURCE_DIR}/../ASM_Sources/bedlam2_data.inc"
        VERBATIM
        COMMENT "Assembling experimental Bedlam 2 x86-64 engine object"
    )
    add_custom_target(bedlam2_engine_x64_experiment
        DEPENDS "${BEDLAM2_X64_ENGINE_OBJECT}")

    if(PS5)
        set_source_files_properties("${BEDLAM2_X64_ENGINE_OBJECT}"
            PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
        add_executable(bedlam2_native
            $<TARGET_OBJECTS:bedlam2_frontend>
            "${BEDLAM2_X64_ENGINE_OBJECT}")
        add_dependencies(bedlam2_native bedlam2_engine_x64_experiment)
        target_link_libraries(bedlam2_native PRIVATE SDL2::SDL2 SceSystemService)
        set_target_properties(bedlam2_native PROPERTIES
            SUFFIX ".elf"
            LINKER_LANGUAGE CXX)

        # Keep the PS5 process startup and allocation operators in this source
        # tree so the native app link does not depend on another project's build
        # directory. These are adapted from the PS5 native-app-kit (GPL-3.0-or-later).
        add_library(bedlam2_native_app_runtime OBJECT
            native_app_crt.cpp
            native_exit.cpp
            native_cpp_runtime.cpp)
        if(BEDLAM2_PS5_TRACE)
            target_compile_definitions(bedlam2_native_app_runtime PRIVATE
                BEDLAM2_PS5_TRACE=1)
        endif()
        target_compile_options(bedlam2_native_app_runtime PRIVATE
            -fno-exceptions
            -fno-rtti)

        set(BEDLAM2_PS5_SDK_ROOT "" CACHE PATH "PS5 Payload SDK root")
        if(NOT BEDLAM2_PS5_SDK_ROOT)
            get_filename_component(_bedlam2_sdk_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            get_filename_component(_bedlam2_sdk_root "${_bedlam2_sdk_bin}" DIRECTORY)
            set(BEDLAM2_PS5_SDK_ROOT "${_bedlam2_sdk_root}" CACHE PATH
                "PS5 Payload SDK root" FORCE)
        endif()

        set(BEDLAM2_PS5_NATIVE_TOOL "" CACHE FILEPATH
            "Path to the PS5 native-app ps5-native-tool executable")
        if(NOT BEDLAM2_PS5_NATIVE_TOOL)
            find_program(_BEDLAM2_PS5_NATIVE_TOOL_DISCOVERED NAMES ps5-native-tool)
            if(_BEDLAM2_PS5_NATIVE_TOOL_DISCOVERED)
                set(BEDLAM2_PS5_NATIVE_TOOL "${_BEDLAM2_PS5_NATIVE_TOOL_DISCOVERED}"
                    CACHE FILEPATH "Path to the PS5 native-app ps5-native-tool executable" FORCE)
            endif()
        endif()

        add_executable(bedlam2_native_pie EXCLUDE_FROM_ALL
            $<TARGET_OBJECTS:bedlam2_native_app_runtime>
            $<TARGET_OBJECTS:bedlam2_frontend>
            "${BEDLAM2_X64_ENGINE_OBJECT}")
        add_dependencies(bedlam2_native_pie bedlam2_engine_x64_experiment)
        # The payload SDK driver adds libkernel_web by default, but that
        # module is not present in native-game processes. Resolve our direct
        # ScePthread bridge imports from the always-loaded libkernel instead.
        target_link_libraries(bedlam2_native_pie PRIVATE
            SDL2::SDL2
            SceSystemService
            "${BEDLAM2_PS5_SDK_ROOT}/target/lib/libkernel.so")
        target_link_options(bedlam2_native_pie PRIVATE
            -nostartfiles
            -T "${CMAKE_CURRENT_SOURCE_DIR}/bedlam2_pie.ld"
            -Wl,--build-id=none
            -Wl,--eh-frame-hdr
            -Wl,--wrap=exit
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/bedlam2_app_symbols.map"
            -Wl,-e,_start
            -Wl,--defsym=kernel_mprotect=0
            -Wl,--defsym=__dlopen=0
            -Wl,--defsym=__dlsym=0
            -Wl,--defsym=__dlclose=0
            -Wl,--defsym=__dladdr=0
            -Wl,--defsym=__dlerror=0)
        set_target_properties(bedlam2_native_pie PROPERTIES
            OUTPUT_NAME "bedlam2_native_pie"
            SUFFIX ".elf"
            LINKER_LANGUAGE CXX)
        set_property(TARGET bedlam2_native_pie APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/bedlam2_pie.ld"
            "${CMAKE_CURRENT_SOURCE_DIR}/bedlam2_app_symbols.map")

        if(BEDLAM2_PS5_NATIVE_TOOL)
            set(BEDLAM2_NATIVE_EBOOT_ELF "${CMAKE_CURRENT_BINARY_DIR}/bedlam2_eboot.elf")
            set(BEDLAM2_NATIVE_EBOOT_BIN "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin")
            add_custom_command(
                OUTPUT "${BEDLAM2_NATIVE_EBOOT_ELF}"
                COMMAND "${BEDLAM2_PS5_NATIVE_TOOL}" link
                    --in "$<TARGET_FILE:bedlam2_native_pie>"
                    --out "${BEDLAM2_NATIVE_EBOOT_ELF}"
                    --stub-dir "${BEDLAM2_PS5_SDK_ROOT}/target/lib"
                    --module-sdk 0x02000009
                    --companion-sdk 0x08050001
                    --file-name eboot.elf
                DEPENDS bedlam2_native_pie "${BEDLAM2_PS5_NATIVE_TOOL}"
                VERBATIM
                COMMENT "Converting Bedlam 2 to the PS5 native executable format"
            )
            add_custom_command(
                OUTPUT "${BEDLAM2_NATIVE_EBOOT_BIN}"
                COMMAND "${BEDLAM2_PS5_NATIVE_TOOL}" self --sign
                    --in "${BEDLAM2_NATIVE_EBOOT_ELF}"
                    --out "${BEDLAM2_NATIVE_EBOOT_BIN}"
                    --magic 0x1D3D154F
                DEPENDS "${BEDLAM2_NATIVE_EBOOT_ELF}"
                VERBATIM
                COMMENT "Wrapping the PS5 native executable as eboot.bin"
            )
            add_custom_target(bedlam2_native_eboot DEPENDS "${BEDLAM2_NATIVE_EBOOT_BIN}")
        else()
            message(STATUS
                "ps5-native-tool not found; set BEDLAM2_PS5_NATIVE_TOOL to build bedlam2_native_eboot")
        endif()
    endif()
endif()

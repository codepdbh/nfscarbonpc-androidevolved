cmake_minimum_required(VERSION 3.22)
project(NFSCAndroidBootstrap C CXX)
if(NOT ANDROID OR NOT (ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_ABI STREQUAL "armeabi-v7a"))
    message(FATAL_ERROR "This target requires Android arm64-v8a or armeabi-v7a")
endif()
# libadrenotools (custom Vulkan drivers) exists for arm64 only.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(NFS_CUSTOM_DRIVERS ON)
else()
    set(NFS_CUSTOM_DRIVERS OFF)
endif()
add_library(native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")
get_filename_component(NFS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PCRECOMP "${NFS_ROOT}/../pcrecomp")
# Shared with the NFSU2 port: its prepared DXVK tree and pinned libadrenotools (sibling checkout).
set(NFSU2_ROOT "${NFS_ROOT}/../nfsu2-recompiled" CACHE PATH "nfsu2-recompiled checkout")
set(DXVK_SOURCE "${NFSU2_ROOT}/build/dxvk-source" CACHE PATH "Prepared DXVK native source")
set(DXVK_ANDROID_STATIC ON CACHE BOOL "Link into application" FORCE)
add_subdirectory("${NFSU2_ROOT}/ports/dxvk_android" "${CMAKE_CURRENT_BINARY_DIR}/dxvk")
set(GEN_DIR "${NFS_ROOT}/src/recomp/gen")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/recomp_*.c")
if(NOT GEN_SOURCES)
    message(FATAL_ERROR "Generate the local game C first; see the project README")
endif()
# Very large translation units leave one compiler working long after all other
# jobs finish. Split only those units, preserving complete functions and headers.
set(ANDROID_GEN_SOURCES)
foreach(source IN LISTS GEN_SOURCES)
    file(SIZE "${source}" source_bytes)
    if(source_bytes GREATER 67108864)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}" "${NFS_ROOT}/scripts/split_android_sources.py")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${NFS_ROOT}/scripts/split_android_sources.py"
            "${source}" "${CMAKE_CURRENT_BINARY_DIR}/split_guest"
            RESULT_VARIABLE split_status OUTPUT_VARIABLE split_sources OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT split_status EQUAL 0)
            message(FATAL_ERROR "Failed to split lifted Android source: ${source}")
        endif()
        string(REPLACE "\n" ";" split_sources "${split_sources}")
        list(APPEND ANDROID_GEN_SOURCES ${split_sources})
    else()
        list(APPEND ANDROID_GEN_SOURCES "${source}")
    endif()
endforeach()
# DLLs lifted as guest modules from copies relocated to their fixed guest
# bases (scripts/prepare_modules.py, layout in guest_modules.inc):
#   server   EA's LAN game server; LAN hosting requires it.
#   d3dx     d3dx9_30.dll: NFSC.exe imports its effect framework and math,
#            and Android has no D3DX.
#   msvcr    msvcr71.dll and msvcp71.dll: Carbon links the Visual C++ 7.1
#   msvcp    runtime dynamically.
#   palmont, logging, pool  NFSCO (scripts/prepare_nfsco.py): the mod, its logger
#            and its hook trampolines.
# Do not silently produce an APK with a missing module.
set(SERVER_GEN_DIR "${NFS_ROOT}/src/recomp/gen_server")
foreach(module server d3dx msvcr msvcp palmont logging pool wsfix)
    file(GLOB module_sources CONFIGURE_DEPENDS "${NFS_ROOT}/src/recomp/gen_${module}/${module}_recomp_*.c")
    if(NOT module_sources)
        message(FATAL_ERROR "No ${module} code in src/recomp/gen_${module}: run scripts/prepare_modules.py")
    endif()
    list(APPEND ANDROID_GEN_SOURCES ${module_sources})
    if(module STREQUAL "server")
        set(SERVER_GEN_SOURCES ${module_sources})
    endif()
endforeach()
# x86 routines of EA's sound banks, lifted from the local Sound/**.abk
# (scripts/abk_module.py + run_lift.py --prefix abk_; see src/runtime/nfs_abk.h).
set(ABK_GEN_DIR "${NFS_ROOT}/src/recomp/gen_abk")
file(GLOB ABK_GEN_SOURCES CONFIGURE_DEPENDS "${ABK_GEN_DIR}/abk_*.c")
if(NOT ABK_GEN_SOURCES)
    message(FATAL_ERROR "No sound-bank code in ${ABK_GEN_DIR}: run scripts/abk_module.py and run_lift.py --prefix abk_")
endif()
list(APPEND ANDROID_GEN_SOURCES ${ABK_GEN_SOURCES})
set_property(GLOBAL PROPERTY JOB_POOLS guest_compile=4)
add_library(nfs_guest OBJECT ${ANDROID_GEN_SOURCES})
set_property(TARGET nfs_guest PROPERTY JOB_POOL_COMPILE guest_compile)
target_include_directories(nfs_guest PRIVATE "${PCRECOMP}/runtime/recomp32" "${GEN_DIR}" "${SERVER_GEN_DIR}" "${NFS_ROOT}/src/runtime")
# RECOMP_LOCAL_REGS: guest registers live in locals of each lifted function
# (written back at every call and return) so they stay in ARM64 registers.
# RECOMP_MEM_QUAL=: guest memory accesses are not volatile; -fno-strict-aliasing
# keeps the untyped guest memory correct, and lifted loops still make an opaque
# call per back-edge. -fwrapv: x86 integer arithmetic wraps. Both validated by
# scripts/difftest_real.py --cc clang-o2 --local-regs (same result as MSVC).
# RECOMP_X87_WINDOW: the x87 stack is a sliding window (push/pop move one
# index instead of shifting 16 values). Every unit of the library must agree,
# so nfsc_android (runtime, overrides, widescreen) defines it too.
target_compile_definitions(nfs_guest PRIVATE RECOMP_STRICT_ICALL RECOMP_LOCAL_REGS RECOMP_X87_WINDOW "RECOMP_MEM_QUAL=")
# Optimize the ARM64 game while avoiding aliasing assumptions in x86 accesses;
# retain unwind tables so unsupported HLE paths can exit to the host cleanly.
target_compile_options(nfs_guest PRIVATE -O2 -w -fwrapv -fno-strict-aliasing -fexceptions -ffp-contract=off
    -include "${CMAKE_CURRENT_SOURCE_DIR}/arm64_guest.h")
# The ~27,700 lifted functions are internal to the library: hidden visibility
# turns their cross-file calls from PLT stubs into direct calls, and ThinLTO
# (as in the MW port) can inline small game functions across the generated
# files, which also removes their guest register save/reload. No debug info:
# it only slows the build of 9M generated lines.
option(NFS_GUEST_LTO "ThinLTO over the lifted game code" ON)
target_compile_options(nfs_guest PRIVATE -g0 -fvisibility=hidden)
# ARMv7: guest data is only as aligned as x86 needed, but the lifted code reads
# floats and doubles through plain pointers, and the compiler would use
# LDRD/STRD/VLDR, which fault on such addresses (SIGBUS BUS_ADRALN). ARM64
# loads tolerate it; on 32-bit assume no alignment for unannotated pointers.
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_options(nfs_guest PRIVATE -fmax-type-align=1)
endif()
if(NFS_GUEST_LTO)
    target_compile_options(nfs_guest PRIVATE -flto=thin)
endif()
# Custom Vulkan drivers (Turnip, ...) imported in the launcher, as in the MW/Carbon ports.
# libadrenotools is pinned in third_party/ (BSD-2-Clause); no driver is bundled.
if(NFS_CUSTOM_DRIVERS)
    add_subdirectory("${NFSU2_ROOT}/third_party/libadrenotools" "${CMAKE_CURRENT_BINARY_DIR}/adrenotools")
endif()
add_library(nfsc_diagnostics SHARED gpu_probe.cpp vulkan_driver.cpp)
target_link_libraries(nfsc_diagnostics PRIVATE android log dl)
target_compile_options(nfsc_diagnostics PRIVATE -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers)
add_library(nfsc_android SHARED main.cpp guest_runtime.cpp d3d9_bridge.cpp vulkan_driver.cpp "${NFS_ROOT}/src/runtime/overrides.c" "${NFS_ROOT}/src/runtime/widescreen.c" "${NFS_ROOT}/src/runtime/abk_code.c" $<TARGET_OBJECTS:nfs_guest>)
target_include_directories(nfsc_android SYSTEM PRIVATE "${DXVK_SOURCE}/include/native"
  "${DXVK_SOURCE}/include/native/windows" "${DXVK_SOURCE}/include/native/directx")
target_include_directories(nfsc_android PRIVATE "${PCRECOMP}/runtime/recomp32" "${PCRECOMP}/runtime/win32hle" "${NFS_ROOT}/src/runtime" "${NFSU2_ROOT}/src/runtime")
target_compile_definitions(nfsc_android PRIVATE RECOMP_STRICT_ICALL RECOMP_X87_WINDOW)
if(SERVER_GEN_SOURCES)
    target_compile_definitions(nfsc_android PRIVATE NFS_SERVER_DLL)
endif()
target_compile_features(nfsc_android PRIVATE cxx_std_17)
target_compile_definitions(nfsc_android PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_compile_options(nfsc_android PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wall -Wextra -Werror -Wno-missing-field-initializers)
target_link_options(nfsc_android PRIVATE "-uANativeActivity_onCreate")
target_link_libraries(nfsc_android PRIVATE native_app_glue android log vulkan dxvk_android aaudio dl)
if(NFS_GUEST_LTO)
    target_link_options(nfsc_android PRIVATE -flto=thin "-Wl,--thinlto-jobs=4"
        "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
endif()
# linkernsbypass has data symbols named like libdl's functions; exporting them breaks
# the system loader when it opens the vendor namespace (MW port note).
# Only adrenotools' archives: excluding ALL would also hide native_app_glue's
# ANativeActivity_onCreate, which NativeActivity looks up by name.
foreach(target nfsc_android nfsc_diagnostics)
    target_link_options(${target} PRIVATE "-Wl,--exclude-libs,libadrenotools.a:liblinkernsbypass.a" "-Wl,-z,max-page-size=16384")
endforeach()
if(NFS_CUSTOM_DRIVERS)
    foreach(target nfsc_android nfsc_diagnostics)
        target_link_libraries(${target} PRIVATE adrenotools)
        target_compile_definitions(${target} PRIVATE NFS_CUSTOM_DRIVERS)
    endforeach()
    target_link_options(hook_impl PRIVATE "-Wl,--exclude-libs,ALL")
    foreach(target adrenotools main_hook file_redirect_hook gsl_alloc_hook hook_impl)
        target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384")
    endforeach()
    # adrenotools opens these from nativeLibraryDir at run time.
    add_dependencies(nfsc_android main_hook file_redirect_hook gsl_alloc_hook hook_impl)
endif()

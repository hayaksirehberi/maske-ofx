cmake_minimum_required(VERSION 3.20)
project(HistoryBrushOFX CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OFX_INCLUDE_DIR "C:/Dev/openfx/include" CACHE PATH "OpenFX SDK include dir")

add_library(HistoryBrush SHARED src/HistoryBrush.cpp)
target_include_directories(HistoryBrush PRIVATE ${OFX_INCLUDE_DIR})
set_target_properties(HistoryBrush PROPERTIES
    PREFIX ""
    SUFFIX ".ofx"
)

if(WIN32)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        target_sources(HistoryBrush PRIVATE src/HistoryBrushCuda.cu)
        target_compile_definitions(HistoryBrush PRIVATE HB_HAS_CUDA)
        set_target_properties(HistoryBrush PROPERTIES
            CUDA_RUNTIME_LIBRARY Static
            # Turing and newer (CUDA 13 dropped pre-Turing); older GPUs use the CPU path
            CUDA_ARCHITECTURES "75;86;89;90;120"
        )
        message(STATUS "CUDA render path: ENABLED")

        add_executable(CudaSmokeTest test/CudaSmokeTest.cpp src/HistoryBrushCuda.cu)
        set_target_properties(CudaSmokeTest PROPERTIES
            CUDA_RUNTIME_LIBRARY Static
            CUDA_ARCHITECTURES "75;86;89;90;120"
        )
        target_include_directories(CudaSmokeTest PRIVATE
            "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3/include")
        target_compile_definitions(CudaSmokeTest PRIVATE HB_HAS_CUDA)
    else()
        message(STATUS "CUDA compiler not found - building CPU-only")
    endif()
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(HistoryBrush PRIVATE src/HistoryBrushMetal.mm)
    set_source_files_properties(src/HistoryBrushMetal.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_compile_definitions(HistoryBrush PRIVATE HB_HAS_METAL)
    target_link_libraries(HistoryBrush PRIVATE "-framework Metal" "-framework Foundation")
    message(STATUS "Metal render path: ENABLED")
endif()

# Assemble the .ofx.bundle layout next to the build output
if(APPLE)
    set(BUNDLE_ARCH_DIR MacOS)
elseif(WIN32)
    set(BUNDLE_ARCH_DIR Win64)
else()
    set(BUNDLE_ARCH_DIR Linux-x86-64)
endif()
set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/HistoryBrush.ofx.bundle/Contents/${BUNDLE_ARCH_DIR})
add_custom_command(TARGET HistoryBrush POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUNDLE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:HistoryBrush> ${BUNDLE_DIR}/HistoryBrush.ofx
)

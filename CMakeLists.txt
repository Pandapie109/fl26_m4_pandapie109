cmake_minimum_required(VERSION 3.16)
project(ai_workspace_m4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC OFF)

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

add_library(aiws_m2 STATIC
    src/document.cpp
    src/workspace.cpp
    src/text_processor.cpp
    src/chunker.cpp
    src/corpus_index.cpp
    src/retrieval_engine.cpp
    src/context_builder.cpp
    src/processing_core.cpp
)
target_include_directories(aiws_m2 PUBLIC include)

add_library(aiws_ipc STATIC src/ipc.cpp)
target_include_directories(aiws_ipc PUBLIC include)
target_link_libraries(aiws_ipc PUBLIC Threads::Threads)

add_library(aiws_gui STATIC
    src/main_window.cpp
    src/async_corpus_build.cpp
    src/workspace_widget.cpp
    src/search_widget.cpp
    src/context_widget.cpp
    src/term_inspector_widget.cpp
    src/search_result_model.cpp
    include/aiws/gui/main_window.hpp
    include/aiws/gui/resettable.hpp
    include/aiws/gui/workspace_widget.hpp
    include/aiws/gui/search_widget.hpp
    include/aiws/gui/context_widget.hpp
    include/aiws/gui/term_inspector_widget.hpp
    include/aiws/gui/search_result_model.hpp
)
set_target_properties(aiws_gui PROPERTIES AUTOMOC ON)
target_include_directories(aiws_gui PUBLIC include)
target_link_libraries(aiws_gui PUBLIC aiws_m2 aiws_ipc Qt6::Widgets Threads::Threads)

add_executable(ai_workspace_m4 src/main.cpp)
target_link_libraries(ai_workspace_m4 PRIVATE aiws_gui)

add_executable(aiws_send tools/aiws_send.cpp)
target_link_libraries(aiws_send PRIVATE aiws_ipc)

include(CTest)
if(BUILD_TESTING)
    add_executable(student_tests tests/student_tests.cpp)
    set_target_properties(student_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(student_tests PRIVATE aiws_gui Qt6::Test)
    add_dependencies(student_tests aiws_send)
    add_test(NAME student_tests COMMAND student_tests)
    add_executable(backend_ipc_tests tests/backend_ipc_tests.cpp)
    target_link_libraries(backend_ipc_tests PRIVATE aiws_m2 aiws_ipc Threads::Threads)
    target_sources(backend_ipc_tests PRIVATE src/async_corpus_build.cpp)
    add_test(NAME backend_ipc_tests COMMAND backend_ipc_tests)
    set_tests_properties(student_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 55)
endif()

if(MSVC)
    foreach(t aiws_m2 aiws_ipc aiws_gui ai_workspace_m4 aiws_send)
        target_compile_options(${t} PRIVATE /W4)
    endforeach()
else()
    foreach(t aiws_m2 aiws_ipc aiws_gui ai_workspace_m4 aiws_send)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()

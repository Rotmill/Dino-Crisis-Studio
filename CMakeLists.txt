cmake_minimum_required(VERSION 3.20)
project(DCME_Studio CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# статическая библиотека C++: приложению не нужен VC++ Redistributable на машине игрока
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SDL2_DIR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../sdl2-msvc/SDL2-2.30.3" CACHE PATH "SDL2 MSVC development package")

set(IMGUI deps/imgui)
set(CORE_SRC src/i18n.cpp src/disc.cpp src/dat.cpp src/mesh.cpp src/room.cpp src/sound.cpp src/synth.cpp src/str.cpp src/mp4.cpp src/script.cpp src/script_edit.cpp
    src/content.cpp src/library.cpp src/gltf.cpp src/gltf_read.cpp src/motions.cpp src/meshsig.cpp src/placement.cpp src/export.cpp src/model_out.cpp src/anim_packs.cpp src/convert.cpp src/pieces.cpp src/species.cpp src/zip.cpp src/packs.cpp src/scenario.cpp src/scn_ops.cpp src/scn_scenes.cpp)
set(CORE_LIBS mfplat mfreadwrite mfuuid ole32)

# консольная версия: экспорт, конвертация, проверки (для автотестов и для тех, кто любит командную строку)
add_executable(DCME_Cli src/cli_main.cpp ${CORE_SRC})
target_include_directories(DCME_Cli PRIVATE src)
target_link_libraries(DCME_Cli PRIVATE ${CORE_LIBS})
target_compile_definitions(DCME_Cli PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(DCME_Cli PRIVATE /utf-8 /MP /W3 /wd4244 /wd4267 /wd4305)
set_target_properties(DCME_Cli PROPERTIES OUTPUT_NAME "DinoCrisisME_Studio_cli")

if(EXISTS "${CMAKE_SOURCE_DIR}/src/main.cpp")
add_executable(DCME_Studio WIN32
    src/main.cpp
    src/ui.cpp
    src/ui_tree.cpp
    src/ui_edit.cpp
    src/ui_view.cpp
    src/ui_packs.cpp
    src/ui_test.cpp
    src/ui_scenario.cpp
    src/view3d.cpp
    ${CORE_SRC}
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/imgui_impl_sdl2.cpp
    ${IMGUI}/imgui_impl_opengl3.cpp
    src/app.rc
)
target_include_directories(DCME_Studio PRIVATE src ${IMGUI} ${SDL2_DIR_ROOT}/include)
target_link_directories(DCME_Studio PRIVATE ${SDL2_DIR_ROOT}/lib/x64)
target_link_libraries(DCME_Studio PRIVATE SDL2 SDL2main opengl32 shell32 ole32 comdlg32 uuid ${CORE_LIBS})
target_compile_definitions(DCME_Studio PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
target_compile_options(DCME_Studio PRIVATE /utf-8 /MP /W3 /wd4244 /wd4267 /wd4305)
set_target_properties(DCME_Studio PROPERTIES OUTPUT_NAME "DinoCrisisME_Studio")
add_custom_command(TARGET DCME_Studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL2_DIR_ROOT}/lib/x64/SDL2.dll" "$<TARGET_FILE_DIR:DCME_Studio>")
endif()

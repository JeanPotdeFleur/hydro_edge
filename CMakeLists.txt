cmake_minimum_required(VERSION 3.10)
project(hydro_edge CXX)

# Définition du standard C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Optimisations matérielles ARMv8 (Pi 5) et multithreading (POSIX)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -pthread")

# Localisation automatique d'OpenCV (fonctionnelle)
find_package(OpenCV REQUIRED)

# Localisation manuelle du SDK Spinnaker
find_path(SPINNAKER_INCLUDE_DIR Spinnaker.h 
    PATHS /opt/spinnaker/include /usr/include/spinnaker /usr/include
)
find_library(SPINNAKER_LIBRARY Spinnaker 
    PATHS /opt/spinnaker/lib /usr/lib /usr/local/lib
)

if(NOT SPINNAKER_INCLUDE_DIR OR NOT SPINNAKER_LIBRARY)
    message(FATAL_ERROR "Spinnaker SDK introuvable. Vérifier le chemin d'installation.")
endif()

message(STATUS "Spinnaker headers trouvés dans : ${SPINNAKER_INCLUDE_DIR}")
message(STATUS "Spinnaker library trouvée dans : ${SPINNAKER_LIBRARY}")

# Short commit hash, captured at configure time and compiled into the binary
# so that every burst manifest records the code version that produced it.
# Re-run cmake after a commit for this to refresh.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE HYDRO_EDGE_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT HYDRO_EDGE_GIT_SHA)
    set(HYDRO_EDGE_GIT_SHA "unknown")
endif()
message(STATUS "Build tagged with commit: ${HYDRO_EDGE_GIT_SHA}")

# Création de l'exécutable
add_executable(hydro_edge main.cpp)
target_compile_definitions(hydro_edge PRIVATE HYDRO_EDGE_GIT_SHA="${HYDRO_EDGE_GIT_SHA}")

# Ajout des chemins d'en-tête (headers)
target_include_directories(hydro_edge PRIVATE ${SPINNAKER_INCLUDE_DIR})

# Édition des liens dynamiques
target_link_libraries(hydro_edge 
    ${OpenCV_LIBS} 
    ${SPINNAKER_LIBRARY} 
    pthread
)

# Diagnostic tool: read-only inventory of the camera front-end.
# Deliberately kept out of the acquisition binary and free of any OpenCV
# dependency, so that it can be built and run without touching the critical
# acquisition path.
add_executable(cam_probe tools/cam_probe.cpp)
target_include_directories(cam_probe PRIVATE ${SPINNAKER_INCLUDE_DIR})
target_link_libraries(cam_probe ${SPINNAKER_LIBRARY})
# Field focus and exposure aid. Third target, kept out of the acquisition
# binary: it runs on one camera at a time, free-running, and the acquisition
# binary refuses to start with fewer than two. Links OpenCV, which the
# acquisition path does not yet exercise.
add_executable(cam_focus tools/cam_focus.cpp)
target_include_directories(cam_focus PRIVATE ${SPINNAKER_INCLUDE_DIR})
target_link_libraries(cam_focus ${OpenCV_LIBS} ${SPINNAKER_LIBRARY})
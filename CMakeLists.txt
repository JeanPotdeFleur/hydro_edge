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

# Création de l'exécutable
add_executable(hydro_edge main.cpp)

# Ajout des chemins d'en-tête (headers)
target_include_directories(hydro_edge PRIVATE ${SPINNAKER_INCLUDE_DIR})

# Édition des liens dynamiques
target_link_libraries(hydro_edge 
    ${OpenCV_LIBS} 
    ${SPINNAKER_LIBRARY} 
    pthread
)
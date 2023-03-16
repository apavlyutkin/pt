cmake_minimum_required( VERSION 3.14 FATAL_ERROR )

project( PostgreTest LANGUAGES C CXX )

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)


set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )


#
# Look for Postrge
#
set( PostgreSQL_ROOT "/usr/local/pgsql" )
find_package( PostgreSQL 14 )
if ( PostgreSQL_FOUND )
    message( STATUS "PostgreSQL_LIBRARIES = " ${PostgreSQL_LIBRARIES} )
    message( STATUS "PostgreSQL_INCLUDE_DIRS = " ${PostgreSQL_INCLUDE_DIRS} )
    message( STATUS "PostgreSQL_LIBRARY_DIRS = " ${PostgreSQL_LIBRARY_DIRS} )
    message( STATUS "PostgreSQL_VERSION_STRING = " ${PostgreSQL_VERSION_STRING} )
    message( STATUS "PostgreSQL_TYPE_INCLUDE_DIR = " ${PostgreSQL_TYPE_INCLUDE_DIR} )
else()
    message( FATAL_ERROR "PostgreSQL NOT found!!!" )
endif()

#
# Add required UtfCpp library
#
find_package( UtfCpp )
if ( UtfCpp_FOUND )
    message( STATUS "UftCpp library is on board!" )
else()
    message(FATAL_ERROR "Unable find/deploy UftCpp library!")
endif()

#
# Add required Howard Hinnant Date library
#
find_package( HowardHinnantDate )
if ( HowardHinnantDate_FOUND )
    message( STATUS "Howard Hinnant Date library is on board!" )
else()
    message( FATAL_ERROR "Unable find/deploy Howard Hinnant Date library!" )
endif()

#
# proceed to the sources
#
add_subdirectory( src )

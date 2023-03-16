cmake_minimum_required( VERSION 3.15 FATAL_ERROR )

#
# create a library
#
add_library( pt SHARED ./pt.cpp )

#
# enable paranoid mode
#
target_compile_options( pt PRIVATE
     $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>: -Werror -Wall -Wno-unused-function>
     $<$<CXX_COMPILER_ID:MSVC>: /WX /Wall>
)

#
# the library MUST be C linked
#
set_target_properties( pt PROPERTIES LINKER_LANGUAGE C )

#
# the library MUST be created as PIC 
#
set_target_properties( pt PROPERTIES POSITION_INDEPENDENT_CODE ON )


#
# add Postgre to include paths (threat Postgre headers as system, so do not apply paranoid mode for them)
#
target_include_directories( pt SYSTEM PRIVATE ${PostgreSQL_INCLUDE_DIRS} )

target_include_directories( pt PRIVATE ${PostgreSQL_LIBRARY_DIRS} )
#
# add dependency on utf8cpp
#
target_link_libraries(pt PRIVATE utf8cpp date pg)
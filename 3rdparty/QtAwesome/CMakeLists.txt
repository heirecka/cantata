set(qtawesome_SRCS QtAwesome.cpp)
set(qtawesome_RCS QtAwesome.qrc)
if (ENABLE_QT5)
    QT5_ADD_RESOURCES(qtawesome_RC_SRCS ${qtawesome_RCS})
else (ENABLE_QT5)
    QT4_ADD_RESOURCES(qtawesome_RC_SRCS ${qtawesome_RCS})
endif (ENABLE_QT5)

include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${QTINCLUDES} )
add_library(qtawesome STATIC ${qtawesome_SRCS} ${qtawesome_RC_SRCS})

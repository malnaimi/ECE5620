
set(SOURCES EtherCAT_app.c)
add_executable(EtherCAT_app ${SOURCES})
target_link_libraries(EtherCAT_app soem)
install(TARGETS EtherCAT_app DESTINATION bin)

add_library(i2s INTERFACE)

target_sources(i2s INTERFACE
	${CMAKE_CURRENT_LIST_DIR}/i2s.c
	${CMAKE_CURRENT_LIST_DIR}/i2s.h
)

target_link_libraries(i2s INTERFACE hardware_pio hardware_clocks)

target_include_directories(i2s INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}
)

pico_generate_pio_header(i2s
  ${CMAKE_CURRENT_LIST_DIR}/audio_i2s.pio
)

ADD_EXECUTABLE(engine_testapp engine_testapp.cc
               mock_server.cc
               mock_server.h
               ${Memcached_SOURCE_DIR}/daemon/protocol/mcbp/engine_errc_2_mcbp.cc
               ${Memcached_SOURCE_DIR}/daemon/protocol/mcbp/engine_errc_2_mcbp.h
               ${Memcached_SOURCE_DIR}/daemon/doc_pre_expiry.cc
               $<TARGET_OBJECTS:memory_tracking>)

TARGET_LINK_LIBRARIES(engine_testapp PRIVATE
		memcached_logger
		mcd_util
		mcd_tracing
		phosphor
		platform
		xattr
		${MALLOC_LIBRARIES}
		${COUCHBASE_NETWORK_LIBS})
INSTALL(TARGETS engine_testapp
        RUNTIME DESTINATION bin)

ADD_TEST(NAME memcached-basic-engine-tests
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
         COMMAND engine_testapp -E default_engine.so
	                        -T basic_engine_testsuite.so)

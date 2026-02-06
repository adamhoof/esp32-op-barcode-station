file(GLOB_RECURSE SOURCES "src/*.cpp")

idf_component_register(
        SRCS ${SOURCES}
        INCLUDE_DIRS "include"
        EMBED_TXTFILES "certs/ca.crt" "certs/client.crt" "certs/client.key"
)

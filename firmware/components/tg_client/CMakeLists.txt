# Only the wire format so far: pure, host-tested, no crypto library and
# no network. The long-poll transport and the mbedtls-backed MAC arrive
# next and are target-only.
idf_component_register(
    SRCS "tg_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES access_core
)

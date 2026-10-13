# The wire format (tg_proto) and the poll policy (tg_poll): both pure,
# host-tested, no crypto library and no network. The long-poll transport
# and the mbedtls-backed MAC arrive next in tg_client.c and are
# target-only.
idf_component_register(
    SRCS "tg_proto.c" "tg_poll.c"
    INCLUDE_DIRS "include"
    REQUIRES access_core
)

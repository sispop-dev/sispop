set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(http_wallet_sources
  files.cpp)

set(http_wallet_headers
  files.h)

# Remove the Monero-specific headers and function calls
# set(http_wallet_private_headers)

# Remove the Monero-specific function calls
# monero_private_headers(http_wallet
#   ${http_wallet_private_headers})

add_library(http_wallet
  ${http_wallet_sources}
  ${http_wallet_headers}
  # ${http_wallet_private_headers}  # Remove this line

  )

target_link_libraries(http_wallet
    PRIVATE
      version)

install(FILES ${http_wallet_headers}
    DESTINATION include/http_wallet)


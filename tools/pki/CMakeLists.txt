find_package(OpenSSL 3.5 REQUIRED)

add_library(chathub_pki STATIC src/dev_ca.cpp)
add_library(chathub::pki ALIAS chathub_pki)

target_compile_features(chathub_pki PUBLIC cxx_std_20)
target_include_directories(chathub_pki PUBLIC include)
target_link_libraries(chathub_pki PUBLIC OpenSSL::SSL OpenSSL::Crypto)

add_executable(chathub-dev-ca src/dev_ca_main.cpp)
target_link_libraries(chathub-dev-ca PRIVATE chathub::pki)
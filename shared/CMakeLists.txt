add_library(chathub_contracts INTERFACE)
add_library(chathub::contracts ALIAS chathub_contracts)

target_compile_features(chathub_contracts INTERFACE cxx_std_20)

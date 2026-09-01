#pragma once
#include "auth/auth_introspection_client.h"
#include "asio.hpp"
namespace auth
{

class AsioAuthIntrospectionClient final : public IAuthIntrospectionClient
{
public:
    AsioAuthIntrospectionClient(asio::io_context& io_context, AuthIntrospectionConfig config);

    IntrospectionRequestPtr introspect(std::string token, IntrospectionHandler handler) override;

private:
    asio::io_context& m_io_context;
    AuthIntrospectionConfig m_config;
};
}

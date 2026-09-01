#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/kazuho-picojson/traits.h>

#include <iostream>

// ==================== 模块：令牌验证配置 ====================
// 功能：保存 JWT 示例用于验证 HS256 签名的开发密钥。
const std::string SECRET_KEY = "chathub-dev-secret";

// ==================== 模块：JWT 验证示例入口 ====================
// 功能：解码内置令牌、验证签名和有效期，并输出其中的用户名。
int main()
{
    const std::string token =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VybmFtZSI6ImFsaWNlIiwiaWF0IjoxNzg1ODIzMzQ4LCJleHAiOjE3ODU4MjY5NDh9."
        "ak-U4eagxXzt1BesIMOxwcg25CfLwF7NVTg2XdlxATA";

    try
    {
        const auto decoded = jwt::decode(token);
        const auto verifier = jwt::verify().allow_algorithm(jwt::algorithm::hs256{SECRET_KEY});
        verifier.verify(decoded);
        const auto username = decoded.get_payload_claim("username").as_string();
        std::cout << "JWT验证通过！ username =" << username << std::endl;
    }
    catch (const std::exception &error)
    {
        std::cout << "JWT验证失败！" << error.what() << std::endl;
    }

    return 0;
}

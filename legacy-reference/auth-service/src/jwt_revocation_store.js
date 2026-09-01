'use strict';

const DEFAULT_KEY_PREFIX = 'chathub:auth:v1'
const JTI_PATTERN = /^[a-zA-Z0-9-._~]{1,128}$/;

function revocationError(code,message,cause){
    const error = new Error(message);
    error.code = code;
    if(cause !== undefined){
        error.cause = cause;
    }
    return error;
}
//封装 Redis 命令错误
async function executeRedisOperation(operation,message) {
    try {
        return await operation();
    } catch (cause) {
        throw revocationError('jwt_revocation_unavailable', message, cause)
    }
}


//------------------------------------------------------------------------//
function createJwtRevocationStore({client, key_prefix = DEFAULT_KEY_PREFIX} = {}) {
    if (!client) {
        throw revocationError(
            'jwt_revocation_config_invalid', 'missing_redis_client'
        );
    }
    if (typeof client.set !== 'function' || typeof client.exists !== 'function') {
        throw revocationError(
            'jwt_revocation_config_invalid', 'invalid_redis_client'
        );
    }
    if (typeof key_prefix !== 'string' || key_prefix.length === 0 ||
        key_prefix !== key_prefix.trim() || key_prefix.endsWith(':')) {
        throw revocationError(
            'jwt_revocation_config_invalid', 'invalid_key_prefix'
        );
    }

    // 验证 jti 是否有效
    function validateJti(jti){
        if(typeof jti !== 'string' || !JTI_PATTERN.test(jti)){
            throw revocationError(
                'jwt_revocation_input_invalid', 'invalid_jti'
            );
        }
        return jti;
    }
    // 创建撤销的 key
    function makeRevokedKey(jti){
        const valid_jti = validateJti(jti);
        return `${key_prefix}:revoked:jti:${valid_jti}`;
    }

    async function revoke(input = {}){
        if(!input || typeof input !== 'object' || Array.isArray(input)){
            throw revocationError(
                'jwt_revocation_input_invalid', 'invalid_revoke_input'
            );
        }

        const {jti, exp, now_seconds} = input;
        const key = makeRevokedKey(jti);

        if(!Number.isSafeInteger(exp) || exp < 0){
            throw revocationError(
                'jwt_revocation_input_invalid', 'invalid_exp'
            );
        }
        if(!Number.isSafeInteger(now_seconds) || now_seconds < 0){
            throw revocationError(
                'jwt_revocation_input_invalid', 'invalid_now_seconds'
            );
        }

        const ttl_seconds = exp - now_seconds;
        //已过期：不访问 Redis
        if(ttl_seconds <= 0){
            return{
                revoked: false, expired: true, created: false, ttl_seconds: 0
            }
        }
        //未过期：SET key 1 EX ttl NX
        const reply = await executeRedisOperation(
            () => client.set(key , '1' , {EX: ttl_seconds , NX: true}),
            'redis_revoke_failed'
        );
        if(reply !== 'OK' && reply !== null){
            throw revocationError(
                'jwt_revocation_data_invariant', 'invalid_revoke_reply'
            );
        }

        return {revoked: true, expired: false, created: reply === 'OK', ttl_seconds: ttl_seconds}
    }
    async function isRevoked(jti) {
        const key = makeRevokedKey(jti);
        const reply = await executeRedisOperation(
            () => client.exists(key),//EXISTS 只关心 key 是否存在
            'redis_is_revoked_failed'
        );
        if(reply !== 0 && reply !== 1){
            throw revocationError(
                'jwt_revocation_data_invariant', 'invalid_is_revoked_reply'
            );
        }
        return reply === 1;
    }

    return {revoke, isRevoked};
}

module.exports = {createJwtRevocationStore};
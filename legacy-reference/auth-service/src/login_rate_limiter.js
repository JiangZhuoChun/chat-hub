'use strict';

const net = require('node:net');

const USERNAME_PATTERN = /^[a-zA-Z0-9_]{3,20}$/;
const MAX_SOURCE_IP_LENGTH = 45;

const DEFAULT_KEY_PREFIX = 'chathub:auth:v1';
const DEFAULT_USER_LIMIT = 5;
const DEFAULT_IP_LIMIT = 20;
const DEFAULT_WINDOW_SECONDS = 60;

function limiterError(code, message, cause) {
    const error = new Error(message);
    error.code = code;
    if (cause !== undefined) {
        error.cause = cause;
    }
    return error;
}
// inspect 与 recordFailure 共享同一份对外结果合同。
function makeLimitResult(usernameResult, sourceIpResult) {
    const dimensions = [usernameResult, sourceIpResult];
    const limited = dimensions.some(
        dimension => dimension.limited
    );

    const retry_after_seconds = dimensions.reduce(
        (currentMax, dimension) => {
            if (!dimension.limited) {
                return currentMax;
            }

            return Math.max(
                currentMax,
                Math.max(1, dimension.ttl_seconds)
            );
        },
        0
    );

    return {
        username: usernameResult,
        source_ip: sourceIpResult,
        limited,
        retry_after_seconds
    };
}
// 只包装 Redis 命令阶段；回复解析错误仍保持 redis_data_invariant。
async function executeRedisOperation(operation, message) {
    try {
        return await operation();
    } catch (cause) {
        throw limiterError(
            'redis_unavailable',
            message,
            cause
        );
    }
}

function createLoginRateLimiter({   client,
                                    keyPrefix = DEFAULT_KEY_PREFIX,
                                    windowSeconds = DEFAULT_WINDOW_SECONDS,
                                    userLimit = DEFAULT_USER_LIMIT,
                                    ipLimit = DEFAULT_IP_LIMIT} = {})
{
    //1. 配置校验
    if(!client){
        throw limiterError(
            'login_limiter_config_invalid',
            'missing_redis_client'
        );
    }
    if(typeof client.multi !== 'function'){
        throw limiterError(
            'login_limiter_config_invalid',
            'invalid_redis_client'
        );
    }
    if(typeof keyPrefix !== 'string' || keyPrefix.length === 0){
        throw limiterError(
            'login_limiter_config_invalid',
            'invalid_key_prefix'
        );
    }
    if(!Number.isInteger(windowSeconds) || windowSeconds <= 0){
        throw limiterError(
            'login_limiter_config_invalid',
            'invalid_window_seconds'
        );
    }
    if(!Number.isInteger(userLimit) || userLimit <= 0){
        throw limiterError(
            'login_limiter_config_invalid',
            'invalid_user_limit'
        );
    }
    if(!Number.isInteger(ipLimit) || ipLimit <= 0){
        throw limiterError(
            'login_limiter_config_invalid',
            'invalid_ip_limit'
        );
    }
    if(keyPrefix !== keyPrefix.trim() || keyPrefix.endsWith(':')){
        throw limiterError(
            'login_limiter_config_invalid',
            'invalid_key_prefix'
        );
    }
    if(ipLimit < userLimit){
        throw limiterError(
            'login_limiter_config_invalid',
            'ip_limit_below_user_limit'
        );
    }

    function validateUsername(username) {
        if(typeof username !== 'string' || !USERNAME_PATTERN.test(username)){
            throw limiterError(
                'login_limiter_input_invalid',
                'invalid_username'
            );
        }

        return username;
    }

    function validateInspectInput(input){
        if(!input || typeof input !== 'object'){
            throw limiterError(
                'login_limiter_input_invalid',
                'missing_login_identity'
            );
        }

        const {username,sourceIp} = input;
        validateUsername(username);
        if(typeof sourceIp !== 'string' || sourceIp.length === 0 ||
            sourceIp.length > MAX_SOURCE_IP_LENGTH || net.isIP(sourceIp) === 0){
            throw limiterError(
                'login_limiter_input_invalid',
                'invalid_source_ip'
            );
        }

        return  {username,sourceIp}
    }

    //2. 写 key 构造函数
    function makeUsernameKey(username){
        return `${keyPrefix}:login-fail:user:${username}`;
    }
    function makeSourceIpKey(sourceIp){
        return `${keyPrefix}:login-fail:ip:${sourceIp}`;
    }

    //解析 Redis 结果”的辅助函数
    function parseDimension(raw_count,ral_ttl,limit){
        if(!Number.isInteger(ral_ttl) || ral_ttl < -2){
            throw limiterError(
                'redis_data_invariant',
                'invalid_redis_ttl'
            );
        }
        if(ral_ttl === -1){
            throw limiterError(
                'redis_data_invariant',
                'login_rate_limit_missing_ttl'
            );
        }
        if(ral_ttl === -2){
            return{
                count: 0,
                ttl_seconds: ral_ttl,
                limited: false
            };
        }

        let count = 0;
        //解析 GET 返回值
        if(raw_count !== null){
            count = Number(raw_count);

            if(!Number.isInteger(count) || count < 0){
                throw limiterError(
                    'redis_data_invariant',
                    'invalid_login_failure_count'
                );
            }
        }
        return{
            count,
            ttl_seconds: ral_ttl,
            limited: count >= limit
        };
    }
    function parseFailureDimension(raw_count,raw_expiry_set, ral_ttl, limit){
        const count = Number(raw_count);
        const expiry_set = Number(raw_expiry_set);
        const ttl_seconds = Number(ral_ttl);

        if (!Number.isSafeInteger(count) || count < 1) {
            throw limiterError(
                'redis_data_invariant',
                'invalid_record_count'
            );
        }
        if (expiry_set !== 0 && expiry_set !== 1) {
            throw limiterError(
                'redis_data_invariant',
                'invalid_expiry_reply'
            );
        }
        if (!Number.isInteger(ttl_seconds) || ttl_seconds < 0) {
            throw limiterError(
                'redis_data_invariant',
                'invalid_record_ttl'
            );
        }

        return{
            count,
            expiry_set,
            ttl_seconds,
            limited: count >= limit
        }
    }

    async function inspect(input = {}){
        const {username,sourceIp} = validateInspectInput(input);

        //生成两个 key
        const username_key =  makeUsernameKey(username);
        const sourceIp_key =  makeSourceIpKey(sourceIp);

        // 一次事务读取两个维度，避免分别读取造成观察窗口不一致。
        const replies = await executeRedisOperation(
            () => client
                .multi()
                .get(username_key)
                .get(sourceIp_key)
                .ttl(username_key)
                .ttl(sourceIp_key)
                .exec(),
            'redis_inspect_failed'
        );

        if(!Array.isArray(replies) || replies.length !== 4){
            throw limiterError(
                'redis_data_invariant',
                'invalid_redis_inspect_reply'
            );
        }

        const username_result = parseDimension(replies[0], replies[2], userLimit);
        const sourceIp_result = parseDimension(replies[1], replies[3], ipLimit);

        return makeLimitResult(username_result, sourceIp_result);
    }
    async function recordFailure(input = {}){
        const {username,sourceIp} = validateInspectInput(input);

        const username_key =  makeUsernameKey(username);
        const sourceIp_key =  makeSourceIpKey(sourceIp);

        // INCR、首次 EXPIRE NX 和 TTL 必须在同一事务中完成。
        const replies = await executeRedisOperation(
            () => client
                .multi()
                .incr(username_key)
                .expire(username_key, windowSeconds, 'NX')
                .ttl(username_key)
                .incr(sourceIp_key)
                .expire(sourceIp_key, windowSeconds, 'NX')
                .ttl(sourceIp_key)
                .exec(),
            'redis_record_failure_failed'
        );

        if(!Array.isArray(replies) || replies.length !== 6){
            throw limiterError(
                'redis_data_invariant',
                'invalid_redis_record_failure_reply'
            );
        }

        const username_result = parseFailureDimension(replies[0],replies[1],replies[2],userLimit);
        const sourceIp_result = parseFailureDimension(replies[3],replies[4],replies[5],ipLimit);

        return makeLimitResult(username_result, sourceIp_result);
    }

    async function clearUserFailures(username) {
        validateUsername(username);

        const username_key = makeUsernameKey(username);

        // 单 key DEL 已经是原子操作，不能先 GET 再 DEL。
        const deleted = await executeRedisOperation(
            () => client.del(username_key),
            'redis_clear_user_failures_failed'
        );

        if(deleted !== 0 && deleted !== 1){
            throw limiterError(
                'redis_data_invariant',
                'invalid_clear_user_failures_reply'
            );
        }

        return {deleted};
    }

    return {inspect,recordFailure,clearUserFailures};
}

module.exports = {createLoginRateLimiter};

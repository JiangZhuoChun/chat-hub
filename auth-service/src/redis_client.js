const {createClient} = require('redis');
// | 参数/属性 | 含义 |
// | `url` | 由运行环境显式提供的 Redis 连接地址 |
// | `connectTimeoutMs` | 建立连接最多等待多少毫秒 |
// | `maxReconnectAttempts` | 最多允许多少次重连尝试 |
// | `reconnectDelayMs` | 每次重连前等待的毫秒数 |
// | `disableOfflineQueue: true` | Redis 断线时，新命令立即失败，不排队等待 |
// | `onError` | 接收 client 生命周期中的错误通知 |
// | `client.isOpen` | TCP socket 是否已打开 |
// | `client.isReady` | client 是否已完成 Redis 协议握手、可以执行命令 |

function configError(message){
    const error = new TypeError(message);
    error.code = 'redis_config_invalid';
    return error;
}

function createRedisClient(config = {}) {

    // 1. 校验 config.url
    if(!config || typeof config.url !== 'string'
        || config.url.trim() === '')
    {
        throw configError('missing_redis_url');
    }

    // 2. 校验三个数值配置
    if(!Number.isFinite(config.connectTimeoutMs) ||
    config.connectTimeoutMs <= 0){
        throw configError('invalid connect timeout');
    }
    if(!Number.isInteger(config.maxReconnectAttempts) ||
    config.maxReconnectAttempts < 0){
        throw configError('invalid max reconnect attempts');
    }
    if(!Number.isFinite(config.reconnectDelayMs) ||
    config.reconnectDelayMs < 0){
        throw configError('invalid reconnect delay');
    }

    // 3. 调用 createClient(...)
    const client =
        createClient({
            url: config.url,
            disableOfflineQueue: true,

            socket:{connectTimeout:config.connectTimeoutMs,
                reconnectStrategy: (retries) =>
                    retries < config.maxReconnectAttempts
                        ? config.reconnectDelayMs
                        : false
            }
        });
    client.on('error',()=>{
        if(typeof config.onError === 'function'){
            config.onError({
                phase: 'runtime',
                code: 'redis_unavailable'
            });
        }
    });

    return client;
}

async function connectRedis(client){
    if(!client || typeof client.connect !== 'function'){
        const error = new Error('invalid_redis_client');
        error.code = 'redis_client_invalid';
        throw error;
    }

    if(client.isReady){
        return client.ping();
    }

    if(client.isOpen){
        const error = new Error('Redis client is open but not ready');
        error.code = 'redis_client_state_invalid';
        throw error;
    }

    try {
        await client.connect();

        const reply = await client.ping();
        if(reply !== 'PONG'){
            throw new Error('Redis PING contract mismatch');
        }
        return reply;
    }catch (error) {
        if(client.isOpen){
            client.destroy();
        }
        throw error;
    }
}
async function closeRedis(client){
    if(!client || typeof client.close !== 'function'){
        const error = new Error('invalid_redis_client');
        error.code = 'redis_client_invalid';
        throw error;
    }
    if(!client.isOpen){
        return;
    }

    try {
        await client.close();
    }catch (error) {
        if(client.isOpen){
            client.destroy();
        }
        throw error;
    }
}
module.exports = {createRedisClient,connectRedis,closeRedis};

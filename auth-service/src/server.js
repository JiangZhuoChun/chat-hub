'use strict';

// ==================== 模块：Auth Service 启动编排 ====================
// 本文件是进程入口，只负责读取配置、连接依赖、组装 HTTP 应用、监听端口和关闭资源。
// 具体的 HTTP 路由在 app.js，Redis 协议操作在 redis_client.js，限流业务在
// login_rate_limiter.js；这样每个模块都有清晰的生命周期边界。
require('dotenv').config({quiet: true});

const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');

const {createRedisClient, connectRedis, closeRedis} = require('./redis_client');
const {createLoginRateLimiter} = require('./login_rate_limiter');
const {createApp} = require('./app');
const {createDatabase} = require('./db');
const {createJwtRevocationStore} = require('./jwt_revocation_store');

// ==================== 模块：进程级配置 ====================
// 这些连接参数是基础设施保护参数，不开放为业务配置，避免启动行为被随意改变。
const DEFAULT_PORT = 3000;
const REDIS_CONNECT_TIMEOUT_MS = 2000;
const REDIS_MAX_RECONNECT_ATTEMPTS = 3;
const REDIS_RECONNECT_DELAY_MS = 250;

const DEFAULT_KEY_PREFIX = 'chathub:auth:v1';
const DEFAULT_USER_LIMIT = 5;
const DEFAULT_IP_LIMIT = 20;
const DEFAULT_WINDOW_SECONDS = 60;

// ==================== 公共辅助：启动配置错误 ====================
// 启动失败只携带稳定 code；调用方可以记录 code，但不应把 Redis URL 或 SECRET_KEY 打到日志。
function startupConfigError(message) {
    const error = new Error(message);
    error.code = 'auth_startup_config_invalid';
    return error;
}

// 把环境变量解析成正整数。环境变量本质上都是字符串，不能直接把任意字符串交给限流器。
function parsePositiveInteger(rawValue, name, fallback) {
    const value = rawValue === undefined ? String(fallback) : String(rawValue).trim();
    if (!/^[0-9]+$/.test(value)) {
        throw startupConfigError(`${name}_invalid`);
    }

    const parsed = Number(value);
    if (!Number.isSafeInteger(parsed) || parsed <= 0) {
        throw startupConfigError(`${name}_invalid`);
    }
    return parsed;
}

// 只读取启动所需的配置，不建立连接，也不产生外部副作用，便于单独测试。
function parseConfig(environment = process.env) {
    if (!environment || typeof environment !== 'object') {
        throw startupConfigError('environment_invalid');
    }

    const redisUrl = typeof environment.CHATHUB_REDIS_URL === 'string'
        ? environment.CHATHUB_REDIS_URL.trim()
        : '';
    if (redisUrl.length === 0) {
        throw startupConfigError('CHATHUB_REDIS_URL_missing');
    }

    const secretKey = typeof environment.SECRET_KEY === 'string'
        ? environment.SECRET_KEY.trim()
        : '';
    if (secretKey.length === 0) {
        throw startupConfigError('SECRET_KEY_missing');
    }

    const internal_service_key = typeof environment.CHATHUB_AUTH_INTERNAL_SERVICE_KEY === 'string'
            ? environment.CHATHUB_AUTH_INTERNAL_SERVICE_KEY.trim()
            : '';
    if (internal_service_key.length === 0) {
        throw startupConfigError('CHATHUB_AUTH_INTERNAL_SERVICE_KEY_missing');
    }

    const keyPrefix = environment.CHATHUB_REDIS_KEY_PREFIX === undefined
        ? DEFAULT_KEY_PREFIX
        : String(environment.CHATHUB_REDIS_KEY_PREFIX).trim();
    if (keyPrefix.length === 0 || keyPrefix.endsWith(':')) {
        throw startupConfigError('CHATHUB_REDIS_KEY_PREFIX_invalid');
    }

    const userLimit = parsePositiveInteger(
        environment.CHATHUB_LOGIN_USER_LIMIT,
        'CHATHUB_LOGIN_USER_LIMIT',
        DEFAULT_USER_LIMIT
    );
    const ipLimit = parsePositiveInteger(
        environment.CHATHUB_LOGIN_IP_LIMIT,
        'CHATHUB_LOGIN_IP_LIMIT',
        DEFAULT_IP_LIMIT
    );
    const windowSeconds = parsePositiveInteger(
        environment.CHATHUB_LOGIN_WINDOW_SECONDS,
        'CHATHUB_LOGIN_WINDOW_SECONDS',
        DEFAULT_WINDOW_SECONDS
    );

    if (ipLimit < userLimit) {
        throw startupConfigError('CHATHUB_LOGIN_IP_LIMIT_below_user_limit');
    }

    return {
        redisUrl,
        secretKey,
        internal_service_key,
        keyPrefix,
        userLimit,
        ipLimit,
        windowSeconds,
        port: DEFAULT_PORT
    };
}

//     Promise 表示“未来某个异步操作的结果”。
//      pending   进行中
//     fulfilled 成功
//     rejected  失败
// ==================== 公共辅助：HTTP 监听生命周期 ====================
// app.listen 是回调 API；包装成 Promise 后，启动阶段可以用 await 表达严格顺序。
function listenApp(app, port) {
    if (!app || typeof app.listen !== 'function') {
        const error = new TypeError('invalid_http_app');
        error.code = 'auth_http_app_invalid';
        return Promise.reject(error);
    }
    if (!Number.isSafeInteger(port) || port <= 0 || port > 65535) {
        const error = new RangeError('invalid_http_port');
        error.code = 'auth_http_port_invalid';
        return Promise.reject(error);
    }

    // 创建一个“未来会成功或失败”的 Promise：
    // Promise 只接受第一次结果
    // - resolve(...)：监听成功；
    // - reject(...)：监听失败。
    return new Promise((resolve, reject) => {
        let settled = false;//表示 Promise 是否已经确定结果
        let httpServer;
        //失败路径
        const onStartupError = error => {
            if (!settled) {
                settled = true;
                reject(error);
            }
        };
        //成功路径
        httpServer = app.listen(port, () => {
            settled = true;
            if (httpServer && typeof httpServer.removeListener === 'function') {
                //移除“启动阶段错误处理器”
                httpServer.removeListener('error', onStartupError);
            }
            resolve(httpServer);
        });
        //once 表示事件触发一次后自动移除监听器
        httpServer.once('error', onStartupError);
    });
}
// HTTP server 没有监听时不调用 close，保证启动失败清理和重复关闭都是安全的。
async function closeHttpServer(httpServer) {
    if (!httpServer || !httpServer.listening) {
        return;
    }

    await new Promise((resolve, reject) => {
        httpServer.close(error => {
            if (error && error.code !== 'ERR_SERVER_NOT_RUNNING') {
                reject(error);
                return;
            }
            resolve();
        });
    });
}

// ==================== 公共辅助：统一资源关闭 ====================
// 关闭顺序：HTTP 停止接收新请求 -> Redis 停止新命令 -> SQLite 释放文件句柄。
// 即使前一个资源关闭失败，也继续尝试后面的资源；最后再抛出第一个错误。
async function closeResources({httpServer, redisClient, db} = {}) {
    let firstError;
    const rememberError = error => {
        if (firstError === undefined) {
            firstError = error;
        }
    };

    try {
        await closeHttpServer(httpServer);
    } catch (error) {
        rememberError(error);
    }

    if (redisClient) {
        try {
            await closeRedis(redisClient);
        } catch (error) {
            rememberError(error);
        }
    }

    if (db && db.open && typeof db.close === 'function') {
        try {
            db.close();
        } catch (error) {
            rememberError(error);
        }
    }

    if (firstError !== undefined) {
        throw firstError;
    }
}

// ==================== 公共入口：组装并启动服务 ====================
// 成功时返回资源句柄和 shutdown 函数，测试或上层宿主可以主动释放它们。
async function main(environment = process.env) {
    const config = parseConfig(environment);
    let redisClient;
    let db;
    let httpServer;

    try {
        // 1. 先创建并连接 Redis；连接失败时绝不监听 HTTP 端口。
        redisClient = createRedisClient({
            url: config.redisUrl,
            connectTimeoutMs: REDIS_CONNECT_TIMEOUT_MS,
            maxReconnectAttempts: REDIS_MAX_RECONNECT_ATTEMPTS,
            reconnectDelayMs: REDIS_RECONNECT_DELAY_MS,
            onError: ({phase, code}) => {
                console.error(`component=redis phase=${phase} event=error code=${code}`);
            }
        });
        await connectRedis(redisClient);

        // 生产运行显式打开 auto.db；测试可调用 createDatabase(tempPath) 得到隔离数据库。
        // 数据库仍在 Redis 连接之后打开，保持统一启动门禁。
        db = createDatabase('auto.db');

        const limiter = createLoginRateLimiter({
            client: redisClient,
            keyPrefix: config.keyPrefix,
            userLimit: config.userLimit,
            ipLimit: config.ipLimit,
            windowSeconds: config.windowSeconds
        });

        const revocation_store = createJwtRevocationStore({
            client: redisClient,
            key_prefix: config.keyPrefix
        })

        const app = createApp({
            db,
            limiter,
            bcrypt,
            jwt,
            secretKey: config.secretKey,
            internal_service_key: config.internal_service_key,
            revocation_store
        });

        // 2. 所有依赖准备完成后，最后才允许 HTTP 对外监听。
        httpServer = await listenApp(app, config.port);
        console.log(`component=auth phase=startup event=listening port=${config.port}`);

        let shutdownPromise;
        const shutdown = signal => {
            if (!shutdownPromise) {
                shutdownPromise = closeResources({httpServer, redisClient, db})
                    .then(() => {
                        console.log(`component=auth phase=shutdown event=closed signal=${signal}`);
                    });
            }
            return shutdownPromise;
        };

        const handleSignal = signal => {
            shutdown(signal).catch(error => {
                console.error(
                    `component=auth phase=shutdown event=failed code=${error.code || 'auth_shutdown_failed'}`
                );
                process.exitCode = 1;
            });
        };
        //'SIGINT'   Ctrl+C
        // 'SIGTERM'   系统要求进程终止
        process.once('SIGINT', () => handleSignal('SIGINT'));
        process.once('SIGTERM', () => handleSignal('SIGTERM'));

        return {httpServer, redisClient, db, shutdown};
    } catch (error) {
        try {
            await closeResources({httpServer, redisClient, db});
        } catch (closeError) {
            // 保留原始启动错误作为主因；清理错误已经完成记录，不覆盖连接失败原因。
            error.closeError = closeError;
        }
        throw error;
    }
}

// ==================== 模块入口保护 ====================
// 被 require 时只导出函数，不自动监听；直接 node server.js 时才启动进程。
if (require.main === module) {
    main().catch(error => {
        console.error(
            `component=auth phase=startup event=failed code=${error.code || 'auth_startup_failed'}`
        );
        process.exitCode = 1;
    });
}

module.exports = {
    closeHttpServer,
    closeResources,
    listenApp,
    main,
    parseConfig
};

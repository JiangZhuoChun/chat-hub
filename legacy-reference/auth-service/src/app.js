'use strict';

// ==================== 模块：Auth Service HTTP 应用工厂 ====================
// 本文件只负责 Express 请求处理和业务调用顺序。
// Redis client、SQLite 连接、bcrypt 与 JWT 实例都从 createApp() 参数注入，
// 因此测试可以替换它们；本文件不负责连接依赖，也不负责 app.listen()。
const express = require('express');
const {randomUUID,timingSafeEqual} = require('node:crypto');

// ==================== 模块：输入、错误和响应常量 ====================
// username 的格式必须与 login_rate_limiter.js 的 key 输入合同保持一致。
const USERNAME_PATTERN = /^[a-zA-Z0-9_]{3,20}$/;
const INTERNAL_SERVICE_KEY_HEADER = 'X-Internal-Service-Key';

// 只有这两类限流依赖错误可以安全映射为 503；其他异常不能伪装成 Redis 故障。
const LIMITER_DEPENDENCY_ERROR_CODES = new Set([
    'redis_unavailable',
    'redis_data_invariant'
]);
const REVOCATION_DEPENDENCY_ERROR_CODES = new Set([
    'jwt_revocation_unavailable',
    'jwt_revocation_data_invariant'
]);

const GENERIC_CREDENTIAL_ERROR = '用户名或密码错误';
const RATE_LIMITED_ERROR = '登录尝试过于频繁，请稍后再试';
const DEPENDENCY_UNAVAILABLE_ERROR = '认证服务暂时不可用';


// ==================== 公共辅助：配置错误 ====================
// 统一创建带稳定 code 的配置错误，供 createApp() 在启动组装阶段抛出。
function appConfigError(message) {
    const error = new Error(message);
    error.code = 'auth_app_config_invalid';
    return error;
}

// 校验依赖对象的最小接口；这里只检查“能否调用”，不创建或连接任何依赖。
function validateDependencies(dependencies = {}) {
    if (!dependencies || typeof dependencies !== 'object') {
        throw appConfigError('dependencies_invalid');
    }

    const {db, limiter, bcrypt, jwt, secretKey,revocation_store,internal_service_key} = dependencies;
    if (!db || typeof db.prepare !== 'function') {
        throw appConfigError('db_dependency_invalid');
    }
    if (!limiter
        || typeof limiter.inspect !== 'function'
        || typeof limiter.recordFailure !== 'function'
        || typeof limiter.clearUserFailures !== 'function') {
        throw appConfigError('limiter_dependency_invalid');
    }
    if (!bcrypt
        || typeof bcrypt.compare !== 'function'
        || typeof bcrypt.hash !== 'function') {
        throw appConfigError('bcrypt_dependency_invalid');
    }
    if (!jwt
        || typeof jwt.sign !== 'function'
        || typeof jwt.verify !== 'function') {
        throw appConfigError('jwt_dependency_invalid');
    }
    if (typeof secretKey !== 'string' || secretKey.length === 0) {
        throw appConfigError('jwt_secret_invalid');
    }
    if(!revocation_store || typeof revocation_store.isRevoked !== 'function' ||
    typeof revocation_store.revoke !== 'function'){
        throw appConfigError('revocation_store_dependency_invalid');
    }
    if(typeof internal_service_key !== 'string' || internal_service_key.length === 0){
        throw appConfigError('internal_service_key_invalid');
    }
}

// ==================== 公共辅助：输入校验 ====================
// 返回 null 表示输入合法；返回中文字符串表示应直接结束为 HTTP 400。
// 该函数无副作用，所以非法输入不会触发 Redis、数据库或 bcrypt。
function validateLoginInput(username, password) {
    if (typeof username !== 'string'
        || typeof password !== 'string'
        || username.length === 0
        || password.length === 0) {
        return '用户名和密码不能为空';
    }
    if (username.length < 3 || username.length > 20) {
        return '用户名长度必须在3-20之间';
    }
    if (password.length < 6 || password.length > 64) {
        return '密码长度必须在6-64之间';
    }
    if (!USERNAME_PATTERN.test(username)) {
        return '用户名只能包含字母、数字和下划线';
    }
    return null;
}

// ==================== 公共辅助：限流错误与 HTTP 映射 ====================
// 判断错误是否来自限流安全依赖，而不是数据库、bcrypt 或 JWT。
function isLimiterDependencyError(error) {
    return Boolean(
        error && LIMITER_DEPENDENCY_ERROR_CODES.has(error.code)
    );
}
function isRevocationDependencyError(error) {
    return Boolean(
        error && REVOCATION_DEPENDENCY_ERROR_CODES.has(error.code)
    );
}

// Redis 无法完成安全判断时必须 fail-closed，不能降级为放行登录。
function sendDependencyUnavailable(res) {
    return res.status(503).json({
        error: DEPENDENCY_UNAVAILABLE_ERROR,
        code: 'authentication_dependency_unavailable'
    });
}

// 将 limiter 的统一结果转换成 429，并同时写入 Retry-After 响应头。
// retry_after_seconds 损坏时返回 503，避免向客户端发送 NaN 或非法倒计时。
function sendRateLimited(res, result) {
    const retryAfterSeconds = Number(result?.retry_after_seconds);
    if (!Number.isSafeInteger(retryAfterSeconds) || retryAfterSeconds < 1) {
        return sendDependencyUnavailable(res);
    }

    res.set('Retry-After', String(retryAfterSeconds));
    return res.status(429).json({
        error: RATE_LIMITED_ERROR,
        code: 'login_rate_limited',
        retry_after_seconds: retryAfterSeconds
    });
}

// 限流调用的错误边界：已知依赖错误转 503，未知错误交给 Express 的 500 处理器。
function handleLimiterError(error, res, next) {
    if (isLimiterDependencyError(error)) {
        return sendDependencyUnavailable(res);
    }
    return next(error);
}
function handleRevocationError(error, res, next) {
    if (isRevocationDependencyError(error)) {
        return sendDependencyUnavailable(res);
    }
    return next(error);
}

function extractBearerToken(req){
    const auth_header = req.headers.authorization;
    if(typeof auth_header !== 'string' || !auth_header.startsWith('Bearer ')){
        return null;
    }

    const token = auth_header.slice('Bearer '.length).trim();
    return token.length > 0 ? token : null;
}
function safeTextEqual(actual_text,expected_text){
    if(typeof actual_text !== 'string' || typeof expected_text !== 'string'){
        return false;
    }
    const actual_buffer = Buffer.from(actual_text,'utf8');
    const expected_buffer = Buffer.from(expected_text,'utf8');

    // timingSafeEqual 要求两个 Buffer 长度相同。
    if(actual_buffer.length !== expected_buffer.length){
        return false;
    }
    return timingSafeEqual(actual_buffer,expected_buffer);
}
function isInternalServiceAuthorized(req, internal_service_key) {
    if(!req || typeof req.get !== 'function'){
        return false;
    }
    const provided_key = req.get(INTERNAL_SERVICE_KEY_HEADER);
    return safeTextEqual(provided_key, internal_service_key);
}
// ==================== 公共入口：createApp ====================
/**
 * 创建不监听端口的 Express 应用。
 *
 * @param {object} dependencies
 * @param {object} dependencies.db 提供 prepare(sql).get(username) 的 SQLite 依赖
 * @param {object} dependencies.limiter 提供 inspect/recordFailure/clearUserFailures
 * @param {object} dependencies.bcrypt 提供 compare(password, hash)
 * @param {object} dependencies.jwt 提供 sign(payload, secret, options)
 * @param {string} dependencies.secretKey JWT 签名密钥
 * @param {object} dependencies.revocation_store 提供 isRevoked(token) 和 revoke(token)
 * @param {string} dependencies.internal_service_key 内部服务密钥
 * @returns {object} 可交给 server.js 或测试监听的 Express app
 */
function createApp(dependencies = {}) {
    validateDependencies(dependencies);

    const {db, limiter, bcrypt,
        jwt, secretKey, revocation_store, internal_service_key} = dependencies;

    const app = express();

    // 让 JSON 请求正文进入 req.body；malformed JSON 由底部错误处理中映射为 400。
    app.use(express.json());

    // ==================== HTTP 路由：POST /register ====================
    // 注册只负责建立持久化用户事实；本步保持旧行为，不把注册请求纳入登录失败限流。
    app.post('/register', async (req, res, next) => {
        const {username, password} = req.body ?? {};
        const inputError = validateLoginInput(username, password);
        if (inputError) {
            return res.status(400).json({error: inputError});
        }

        let passwordHash;
        try {
            passwordHash = await bcrypt.hash(password, 10);
        } catch (error) {
            return next(error);
        }

        try {
            db.prepare(
                'INSERT INTO users (username, password) VALUES (?, ?)'
            ).run(username, passwordHash);
        } catch (error) {
            if (error && error.code === 'SQLITE_CONSTRAINT_UNIQUE') {
                return res.status(409).json({error: '用户名已存在'});
            }
            return next(error);
        }

        return res.status(201).json({message: '注册成功'});
    });

    // ==================== HTTP 路由：POST /login ====================
    app.post('/login', async (req, res, next) => {
        // 第一关：只校验结构，不把非法请求计入失败次数。
        const {username, password} = req.body ?? {};
        const inputError = validateLoginInput(username, password);
        if (inputError) {
            return res.status(400).json({error: inputError});
        }

        // 第二关：使用底层连接地址作为可信 source IP。
        // 未配置可信反向代理时，不能读取客户端可伪造的 X-Forwarded-For。
        const sourceIp = req.socket?.remoteAddress;
        if (typeof sourceIp !== 'string' || sourceIp.length === 0) {
            return sendDependencyUnavailable(res);
        }

        // 第三关：先检查两个 Redis 限流维度，再允许进入数据库和 bcrypt。
        let inspectResult;
        try {
            inspectResult = await limiter.inspect({username, sourceIp});
        } catch (error) {
            return handleLimiterError(error, res, next);
        }

        if (inspectResult.limited) {
            return sendRateLimited(res, inspectResult);
        }

        // 第四关：限流通过后才查询持久化用户事实并比较密码哈希。
        let user;
        try {
            user = db
                .prepare('SELECT * FROM users WHERE username = ?')
                .get(username);
        } catch (error) {
            return next(error);
        }

        let credentialsOk = false;
        if (user) {
            try {
                credentialsOk = await bcrypt.compare(password, user.password);
            } catch (error) {
                return next(error);
            }
        }

        // 凭据错误仍使用统一文案；用户不存在和密码错误都记录两个维度的失败。
        if (!credentialsOk) {
            let failureResult;
            try {
                failureResult = await limiter.recordFailure({
                    username,
                    sourceIp
                });
            } catch (error) {
                return handleLimiterError(error, res, next);
            }

            if (failureResult.limited) {
                return sendRateLimited(res, failureResult);
            }
            return res.status(401).json({error: GENERIC_CREDENTIAL_ERROR});
        }

        // 凭据正确只清理 username key，不清理 source IP 的累计失败。
        // 清理成功是签发 token 的前置条件，避免清理失败时放行登录。
        try {
            await limiter.clearUserFailures(username);
        } catch (error) {
            return handleLimiterError(error, res, next);
        }

        // 最后才签发一小时 JWT；token 不进入 Redis，也不写入日志。
        try {
            const jti = randomUUID();
            const token = jwt.sign(
                {username},
                secretKey,
                {
                    expiresIn: '1h',
                    jwtid: jti
                }
            );
            return res.status(200).json({
                message: '登录成功',
                username,
                token
            });
        } catch (error) {
            return next(error);
        }
    });

    app.post('/internal/auth/introspect',async (req,res,next) =>{
        // 第一关：确认调用方是 ChatServer。
        if(!isInternalServiceAuthorized(req,internal_service_key)){
            return res.status(401).json({
                error: '内部服务认证失败',
                code: 'internal_service_rejected'
            })
        }
        // 第二关：确认内部接口请求格式正确。
        const request_body = req.body ?? {};
        if(!request_body
            || typeof request_body !== 'object'
            || Array.isArray(request_body)
            ||typeof request_body.token !== 'string'
            || request_body.token.trim().length === 0){
            return res.status(400).json({
                error: '无效的 introspection 请求',
                code: 'invalid_introspection_request'
            });
        }
        let decoded_token;
        try {
            // 这里不能使用 ignoreExpiration。
            decoded_token = jwt.verify(request_body.token.trim(), secretKey);
        }catch (error){
            return res.status(401).json({
                error: '无效的授权信息',
                code: 'authentication_rejected'
            });
        }
        // JWT 签名正确，还要检查业务 claims。
        if(!decoded_token
            || typeof decoded_token !== 'object'
            || typeof decoded_token.username !== 'string'
            || decoded_token.username.length === 0
            || typeof decoded_token.jti !== 'string'
            || decoded_token.jti.length === 0
            || !Number.isSafeInteger(decoded_token.exp)) {
            return res.status(401).json({
                error: '无效的授权信息',
                code: 'authentication_rejected'
            });
        }

        let is_revoked;
        try {
            is_revoked = await revocation_store.isRevoked(decoded_token.jti);
        }catch (error){
            return handleRevocationError(error,res,next);
        }

        if(is_revoked){
            return res.status(401).json({
            error: '无效的授权信息',
            code: 'authentication_rejected'
        })}
        return res.status(200).json({
            active: true,
            username: decoded_token.username
        })
    })

    // ==================== HTTP 路由：GET /me ====================
    // /me 先验证 JWT，再查询撤销状态；不查询数据库，也不参与登录限流。
    app.get('/me', async (req, res, next) => {
        const token = extractBearerToken(req);
        if (!token) {
            return res.status(401).json({
                error: '未提供有效的授权信息'
            });
        }

        try {
            const decoded_token = jwt.verify(token, secretKey);
            //JWT 验证成功后，不要马上返回 200。先检查 claims
            if (!decoded_token ||
                typeof decoded_token !== 'object' ||
                typeof decoded_token.username !== 'string' ||
                typeof decoded_token.jti !== 'string' ||
                !Number.isSafeInteger(decoded_token.exp)) {
                return res.status(401).json({
                    error: '无效的授权信息',
                    code: 'authentication_rejected'
                });
            }
            //然后查询 Redis
            let is_revoked;
            try {
                is_revoked = await revocation_store.isRevoked(decoded_token.jti);
            }catch (error){
                return handleRevocationError(error,res,next);
            }
            if(is_revoked){
                return res.status(401).json({
                    error: '无效的授权信息',
                    code: 'authentication_rejected'
                })
            }
            //最后才返回成功
            return res.status(200).json({
                username: decoded_token.username
            });
        } catch (error) {
            if (error && error.name === 'TokenExpiredError') {
                return res.status(401).json({
                    error: '授权已过期'
                });
            }
            return res.status(401).json({
                error: '无效的授权信息'
            });
        }
    });

    app.post('/logout',async (req,res,next) =>{
        const token = extractBearerToken(req);
        if (!token) {
            return res.status(401).json({
                error: '未提供有效的授权信息'
            });
        }

        let decoded_token;
        try {
            decoded_token = jwt.verify(
                token,
                secretKey,
                {
                    ignoreExpiration: true  //关闭 JWT 的“过期检查
                });
        }catch (error){
            return res.status(401).json({
                error: '无效的授权信息',
                code: 'authentication_rejected'
            });
        }

        if (
            !decoded_token ||
            typeof decoded_token !== 'object' ||
            typeof decoded_token.jti !== 'string' ||
            !Number.isSafeInteger(decoded_token.exp)
        ) {
            return res.status(401).json({
                error: '无效的授权信息',
                code: 'authentication_rejected'
            });
        }

        const now_seconds = Math.floor(Date.now()/1000);
        try {
            await revocation_store.revoke({
                jti: decoded_token.jti,
                exp: decoded_token.exp,
                now_seconds
            })
        }catch (error){
            return handleRevocationError(error,res,next);
        }

        return res.status(200).json({
            message: '退出登录成功'
        });
    })

    // ==================== 公共错误处理中间件 ====================
    // Express 5 会把 async 路由的 reject 交给这里；不向客户端泄露内部错误细节。
    /** @type {import('express').ErrorRequestHandler} */
    const error_handler = (error, _req, res, next) => {
        if (res.headersSent) {
            return next(error);
        }
        if (error && error.type === 'entity.parse.failed') {
            return res.status(400).json({
                error: '请求正文必须是有效 JSON'
            });
        }
        return res.status(500).json({
            error: '服务器内部错误'
        });
    };
    app.use(error_handler);

    // 工厂只返回 app；端口监听和依赖生命周期由 server.js 统一管理。
    return app;
}

// ==================== 公共导出 ====================
module.exports = {createApp};

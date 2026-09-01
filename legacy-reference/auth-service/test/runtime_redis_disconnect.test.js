'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {randomUUID} = require('node:crypto');
const {mkdtemp, rm} = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');

const {createRedisClient, connectRedis, closeRedis} = require('../src/redis_client');
const {createLoginRateLimiter} = require('../src/login_rate_limiter');
const {createJwtRevocationStore} = require('../src/jwt_revocation_store');
const {createApp} = require('../src/app');
const {createDatabase} = require('../src/db');
const {closeHttpServer} = require('../src/server');
const {requestJson} = require('./http_test_helpers');

const REDIS_URL = process.env.CHATHUB_REDIS_TEST_URL;
if (typeof REDIS_URL !== 'string' || REDIS_URL.trim().length === 0) {
    throw new Error('CHATHUB_REDIS_TEST_URL_missing');
}

function listenOnLoopback(app) {
    return new Promise((resolve, reject) => {
        let http_server;
        const on_startup_error = error => {
            if (http_server) {
                http_server.removeListener('error', on_startup_error);
            }
            reject(error);
        };

        http_server = app.listen(0, '127.0.0.1', () => {
            http_server.removeListener('error', on_startup_error);
            resolve(http_server);
        });
        http_server.once('error', on_startup_error);
    });
}

function jsonRequest(method, body) {
    return {
        method,
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(body)
    };
}

function makeApp({db, redis_client, key_prefix, secret_key, internal_service_key}) {
    const limiter = createLoginRateLimiter({
        client: redis_client,
        keyPrefix: key_prefix,
        userLimit: 5,
        ipLimit: 20,
        windowSeconds: 60
    });
    const revocation_store = createJwtRevocationStore({
        client: redis_client,
        key_prefix
    });

    return createApp({
        db,
        limiter,
        bcrypt,
        jwt,
        secretKey: secret_key,
        revocation_store,
        internal_service_key
    });
}

test('当真实 Redis 在运行期断开并恢复时，登录应先 fail-closed 后恢复', {timeout: 15000}, async t => {
    const run_id = randomUUID().replace(/-/g, '');
    const username = `runtime_${run_id.slice(0, 12)}`;
    const password = 'password123';
    const key_prefix = `chathub:test:runtime-disconnect:${run_id}`;
    const secret_key = `runtime-secret-${run_id}`;
    const internal_service_key = `runtime-internal-${run_id}`;

    const temp_directory = await mkdtemp(path.join(os.tmpdir(), 'chathub-auth-runtime-disconnect-'));
    const db = createDatabase(path.join(temp_directory, 'auth.db'));
    let redis_client = createRedisClient({
        url: REDIS_URL.trim(),
        connectTimeoutMs: 2000,
        maxReconnectAttempts: 0,
        reconnectDelayMs: 0
    });
    let http_server;

    t.after(async () => {
        // 关闭函数允许 client 已被 destroy；此处只负责收尾，不掩盖主体断言。
        if (http_server) {
            await closeHttpServer(http_server);
        }
        if (redis_client && redis_client.isOpen) {
            await closeRedis(redis_client);
        }
        if (db.open) {
            db.close();
        }
        await rm(temp_directory, {recursive: true, force: true});
    });

    await connectRedis(redis_client);
    http_server = await listenOnLoopback(makeApp({
        db,
        redis_client,
        key_prefix,
        secret_key,
        internal_service_key
    }));
    const first_base_url = `http://127.0.0.1:${http_server.address().port}`;

    const registration = await requestJson(
        first_base_url,
        '/register',
        jsonRequest('POST', {username, password})
    );
    assert.deepEqual(registration, {
        status: 201,
        body: {message: '注册成功'}
    });

    // destroy() 模拟运行期连接已经失效；该服务按既定策略不自动无限重连。
    redis_client.destroy();
    const disconnected_login = await requestJson(
        first_base_url,
        '/login',
        jsonRequest('POST', {username, password})
    );
    assert.deepEqual(disconnected_login, {
        status: 503,
        body: {
            error: '认证服务暂时不可用',
            code: 'authentication_dependency_unavailable'
        }
    });

    await closeHttpServer(http_server);
    http_server = undefined;

    // 恢复路径是“重新创建依赖并重启 HTTP 入口”，而不是把旧 client 当成已恢复。
    redis_client = createRedisClient({
        url: REDIS_URL.trim(),
        connectTimeoutMs: 2000,
        maxReconnectAttempts: 0,
        reconnectDelayMs: 0
    });
    await connectRedis(redis_client);
    http_server = await listenOnLoopback(makeApp({
        db,
        redis_client,
        key_prefix,
        secret_key,
        internal_service_key
    }));
    const recovered_base_url = `http://127.0.0.1:${http_server.address().port}`;

    const recovered_login = await requestJson(
        recovered_base_url,
        '/login',
        jsonRequest('POST', {username, password})
    );
    assert.equal(recovered_login.status, 200);
    assert.equal(recovered_login.body.username, username);
    assert.equal(typeof recovered_login.body.token, 'string');
    assert.ok(recovered_login.body.token.length > 0);
});

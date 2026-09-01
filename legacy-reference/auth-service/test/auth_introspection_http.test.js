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

function jsonRequestBody(body) {
    return {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(body)
    };
}

async function closeResources({http_server, redis_client, db, temp_directory, redis_keys = []}) {
    let first_error;
    const remember_error = error => {
        if (first_error === undefined) {
            first_error = error;
        }
    };

    try {
        await closeHttpServer(http_server);
    } catch (error) {
        remember_error(error);
    }

    if (redis_client && redis_client.isOpen && redis_keys.length > 0) {
        try {
            await redis_client.del(redis_keys);
        } catch (error) {
            remember_error(error);
        }
    }

    try {
        await closeRedis(redis_client);
    } catch (error) {
        remember_error(error);
    }

    try {
        if (db && db.open) {
            db.close();
        }
    } catch (error) {
        remember_error(error);
    }

    try {
        if (temp_directory) {
            await rm(temp_directory, {recursive: true, force: true});
        }
    } catch (error) {
        remember_error(error);
    }

    if (first_error !== undefined) {
        throw first_error;
    }
}

async function createFixture() {
    const run_id = randomUUID().replace(/-/g, '');
    const key_prefix = `chathub:test:introspection:${run_id}`;
    const secret_key = `introspection-secret-${run_id}`;
    const internal_service_key = `introspection-internal-${run_id}`;
    const username = `intro_${run_id.slice(0, 12)}`;
    const jti = `jti-${run_id}`;
    const temp_directory = await mkdtemp(path.join(os.tmpdir(), 'chathub-auth-introspection-'));
    const db = createDatabase(path.join(temp_directory, 'auth.db'));
    const redis_client = createRedisClient({
        url: REDIS_URL.trim(),
        connectTimeoutMs: 2000,
        maxReconnectAttempts: 0,
        reconnectDelayMs: 0
    });
    await connectRedis(redis_client);

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
    const app = createApp({
        db,
        limiter,
        bcrypt,
        jwt,
        secretKey: secret_key,
        revocation_store,
        internal_service_key
    });
    const http_server = await listenOnLoopback(app);
    const base_url = `http://127.0.0.1:${http_server.address().port}`;
    const token = jwt.sign(
        {username},
        secret_key,
        {expiresIn: 120, jwtid: jti}
    );

    return {
        base_url,
        db,
        http_server,
        internal_service_key,
        jti,
        key_prefix,
        redis_client,
        redis_keys: [`${key_prefix}:revoked:jti:${jti}`],
        secret_key,
        temp_directory,
        token,
        username
    };
}

test('当 introspection 凭证或请求正文不合法时应在门禁拒绝，合法请求应返回身份', {timeout: 15000}, async t => {
    const fixture = await createFixture();
    t.after(() => closeResources(fixture));

    let verify_calls = 0;
    const observed_jwt = {
        sign: jwt.sign,
        verify: (...args) => {
            verify_calls += 1;
            return jwt.verify(...args);
        }
    };
    const app = createApp({
        db: fixture.db,
        limiter: createLoginRateLimiter({
            client: fixture.redis_client,
            keyPrefix: `${fixture.key_prefix}:observed`,
            userLimit: 5,
            ipLimit: 20,
            windowSeconds: 60
        }),
        bcrypt,
        jwt: observed_jwt,
        secretKey: fixture.secret_key,
        revocation_store: createJwtRevocationStore({
            client: fixture.redis_client,
            key_prefix: `${fixture.key_prefix}:observed`
        }),
        internal_service_key: fixture.internal_service_key
    });
    const http_server = await listenOnLoopback(app);
    t.after(() => closeHttpServer(http_server));
    const base_url = `http://127.0.0.1:${http_server.address().port}`;

    const active = await requestJson(base_url, '/internal/auth/introspect', {
        ...jsonRequestBody({token: fixture.token}),
        headers: {
            'Content-Type': 'application/json',
            'X-Internal-Service-Key': fixture.internal_service_key
        }
    });
    assert.deepEqual(active, {
        status: 200,
        body: {active: true, username: fixture.username}
    });
    assert.equal(verify_calls, 1);

    const verify_calls_before_rejected_service = verify_calls;
    const rejected_service = await requestJson(base_url, '/internal/auth/introspect', {
        ...jsonRequestBody({token: fixture.token}),
        headers: {
            'Content-Type': 'application/json',
            'X-Internal-Service-Key': 'wrong-internal-key'
        }
    });
    assert.deepEqual(rejected_service, {
        status: 401,
        body: {error: '内部服务认证失败', code: 'internal_service_rejected'}
    });
    assert.equal(verify_calls, verify_calls_before_rejected_service);

    const wrong_key_and_bad_body = await requestJson(base_url, '/internal/auth/introspect', {
        ...jsonRequestBody({}),
        headers: {
            'Content-Type': 'application/json',
            'X-Internal-Service-Key': 'wrong-internal-key'
        }
    });
    assert.equal(wrong_key_and_bad_body.status, 401);
    assert.equal(wrong_key_and_bad_body.body.code, 'internal_service_rejected');

    const invalid_request = await requestJson(base_url, '/internal/auth/introspect', {
        ...jsonRequestBody({}),
        headers: {
            'Content-Type': 'application/json',
            'X-Internal-Service-Key': fixture.internal_service_key
        }
    });
    assert.deepEqual(invalid_request, {
        status: 400,
        body: {error: '无效的 introspection 请求', code: 'invalid_introspection_request'}
    });

    const invalid_token = await requestJson(base_url, '/internal/auth/introspect', {
        ...jsonRequestBody({token: 'not-a-jwt'}),
        headers: {
            'Content-Type': 'application/json',
            'X-Internal-Service-Key': fixture.internal_service_key
        }
    });
    assert.deepEqual(invalid_token, {
        status: 401,
        body: {error: '无效的授权信息', code: 'authentication_rejected'}
    });

    const old_token = jwt.sign(
        {username: fixture.username},
        fixture.secret_key,
        {expiresIn: 120}
    );
    const old_token_result = await requestJson(base_url, '/internal/auth/introspect', {
        ...jsonRequestBody({token: old_token}),
        headers: {
            'Content-Type': 'application/json',
            'X-Internal-Service-Key': fixture.internal_service_key
        }
    });
    assert.deepEqual(old_token_result, {
        status: 401,
        body: {error: '无效的授权信息', code: 'authentication_rejected'}
    });

    await closeHttpServer(http_server);
});

test('当 token 退出或已过期时，logout 应保持撤销幂等并避免无效写入', {timeout: 15000}, async t => {
    const fixture = await createFixture();
    t.after(() => closeResources(fixture));

    const before_logout = await requestJson(fixture.base_url, '/me', {
        headers: {Authorization: `Bearer ${fixture.token}`}
    });
    assert.deepEqual(before_logout, {
        status: 200,
        body: {username: fixture.username}
    });

    const logout = await requestJson(fixture.base_url, '/logout', {
        ...jsonRequestBody({}),
        headers: {Authorization: `Bearer ${fixture.token}`}
    });
    assert.deepEqual(logout, {
        status: 200,
        body: {message: '退出登录成功'}
    });

    const revoked_key = fixture.redis_keys[0];
    assert.equal(await fixture.redis_client.exists(revoked_key), 1);
    const revoked_ttl = await fixture.redis_client.ttl(revoked_key);
    assert.ok(revoked_ttl > 0 && revoked_ttl <= 120);

    const after_logout = await requestJson(fixture.base_url, '/me', {
        headers: {Authorization: `Bearer ${fixture.token}`}
    });
    assert.deepEqual(after_logout, {
        status: 401,
        body: {error: '无效的授权信息', code: 'authentication_rejected'}
    });

    const introspection_after_logout = await requestJson(
        fixture.base_url,
        '/internal/auth/introspect',
        {
            ...jsonRequestBody({token: fixture.token}),
            headers: {
                'Content-Type': 'application/json',
                'X-Internal-Service-Key': fixture.internal_service_key
            }
        }
    );
    assert.deepEqual(introspection_after_logout, {
        status: 401,
        body: {error: '无效的授权信息', code: 'authentication_rejected'}
    });

    const repeated_logout = await requestJson(fixture.base_url, '/logout', {
        ...jsonRequestBody({}),
        headers: {Authorization: `Bearer ${fixture.token}`}
    });
    assert.deepEqual(repeated_logout, logout);

    const expired_jti = `expired-${fixture.jti}`;
    const expired_token = jwt.sign(
        {username: fixture.username},
        fixture.secret_key,
        {expiresIn: -10, jwtid: expired_jti}
    );
    const expired_logout = await requestJson(fixture.base_url, '/logout', {
        ...jsonRequestBody({}),
        headers: {Authorization: `Bearer ${expired_token}`}
    });
    assert.deepEqual(expired_logout, {
        status: 200,
        body: {message: '退出登录成功'}
    });
    assert.equal(
        await fixture.redis_client.exists(`${fixture.key_prefix}:revoked:jti:${expired_jti}`),
        0
    );

    const expired_me = await requestJson(fixture.base_url, '/me', {
        headers: {Authorization: `Bearer ${expired_token}`}
    });
    assert.deepEqual(expired_me, {
        status: 401,
        body: {error: '授权已过期'}
    });

    const missing_jti_logout = await requestJson(fixture.base_url, '/logout', {
        ...jsonRequestBody({}),
        headers: {
            Authorization: `Bearer ${jwt.sign(
                {username: fixture.username},
                fixture.secret_key,
                {expiresIn: 120}
            )}`
        }
    });
    assert.deepEqual(missing_jti_logout, {
        status: 401,
        body: {error: '无效的授权信息', code: 'authentication_rejected'}
    });
});

test('当 Redis 撤销查询不可用时，/me、introspection 和 logout 应返回 503', {timeout: 15000}, async t => {
    const fixture = await createFixture();
    t.after(() => closeResources(fixture));

    fixture.redis_client.destroy();

    const expected = {
        status: 503,
        body: {
            error: '认证服务暂时不可用',
            code: 'authentication_dependency_unavailable'
        }
    };

    const me_result = await requestJson(fixture.base_url, '/me', {
        headers: {Authorization: `Bearer ${fixture.token}`}
    });
    assert.deepEqual(me_result, expected);

    const introspection_result = await requestJson(
        fixture.base_url,
        '/internal/auth/introspect',
        {
            ...jsonRequestBody({token: fixture.token}),
            headers: {
                'Content-Type': 'application/json',
                'X-Internal-Service-Key': fixture.internal_service_key
            }
        }
    );
    assert.deepEqual(introspection_result, expected);

    const logout_result = await requestJson(fixture.base_url, '/logout', {
        ...jsonRequestBody({}),
        headers: {Authorization: `Bearer ${fixture.token}`}
    });
    assert.deepEqual(logout_result, expected);
});

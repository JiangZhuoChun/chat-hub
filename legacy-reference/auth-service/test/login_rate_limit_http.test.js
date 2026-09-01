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
const {createApp} = require('../src/app');
const {createDatabase} = require('../src/db');
const {createJwtRevocationStore} = require('../src/jwt_revocation_store');
const {closeHttpServer} = require('../src/server');
const {requestJson, requestJsonWithRetryAfter} = require('./http_test_helpers');

const redisUrl = process.env.CHATHUB_REDIS_TEST_URL;

if (typeof redisUrl !== 'string' || redisUrl.trim().length === 0) {
    throw new Error('CHATHUB_REDIS_TEST_URL_missing');
}

// 功能：让操作系统分配一个空闲端口，并把“开始监听”转换为可 await 的 Promise。
function listenOnLoopback(app) {
    return new Promise((resolve, reject) => {
        let httpServer;

        const onStartupError = error => {
            if (httpServer) {
                httpServer.removeListener('error', onStartupError);
            }
            reject(error);
        };

        httpServer = app.listen(0, '127.0.0.1', () => {
            httpServer.removeListener('error', onStartupError);
            resolve(httpServer);
        });
        httpServer.once('error', onStartupError);
    });
}

// 功能：在明确截止时间内轮询 Redis，确认 username/IP 两个失败 key 都已过期。
async function waitForNoFailures(limiter, identity, {
    timeoutMs = 6000,
    intervalMs = 100
} = {}) {
    const deadline = Date.now() + timeoutMs;

    while (Date.now() <= deadline) {
        const state = await limiter.inspect(identity);
        if (state.username.ttl_seconds === -2
            && state.source_ip.ttl_seconds === -2) {
            return state;
        }

        const remainingMs = deadline - Date.now();
        if (remainingMs <= 0) {
            break;
        }

        await new Promise(resolve => {
            setTimeout(resolve, Math.min(intervalMs, remainingMs));
        });
    }

    throw new Error('login_failure_expiry_timeout');
}

// 功能：按依赖关系的反方向清理资源；即使某一步失败，后续资源也必须继续释放。
async function closeTestResources({httpServer, redisClient, redisKeys, db, tempDirectory}) {
    let firstError;

    const rememberError = error => {
        if (!firstError) {
            firstError = error;
        }
    };

    if (httpServer) {
        try {
            await closeHttpServer(httpServer);
        } catch (error) {
            rememberError(error);
        }
    }

    if (redisClient && redisClient.isOpen && redisKeys.length > 0) {
        try {
            await redisClient.del(redisKeys);
        } catch (error) {
            rememberError(error);
        }
    }

    if (redisClient) {
        try {
            await closeRedis(redisClient);
        } catch (error) {
            rememberError(error);
        }
    }

    if (db) {
        try {
            db.close();
        } catch (error) {
            rememberError(error);
        }
    }

    if (tempDirectory) {
        try {
            await rm(tempDirectory, {recursive: true, force: true});
        } catch (error) {
            rememberError(error);
        }
    }

    if (firstError) {
        throw firstError;
    }
}

test('当真实 Redis、临时 SQLite 和 HTTP 协同运行时，登录限流应执行双维度门禁', {timeout: 20000}, async t => {
    const runId = randomUUID().replace(/-/g, '');
    const username = `http_test_${runId.slice(0, 8)}`;
    const password = 'password123';
    const usernameB = `http_test_b_${runId.slice(0, 8)}`;
    const passwordB = 'password456';
    const usernameC = `http_test_c_${runId.slice(0, 8)}`;
    const passwordC = 'password789';
    const usernameUnknown = `http_u_${runId.slice(0, 8)}`;
    const sourceIp = '127.0.0.1';
    const keyPrefix = `chathub:test:http:${runId}`;
    const secretKey = `test-secret-${runId}`;
    const redisKeys = [
        `${keyPrefix}:login-fail:user:${username}`,
        `${keyPrefix}:login-fail:user:${usernameB}`,
        `${keyPrefix}:login-fail:user:${usernameC}`,
        `${keyPrefix}:login-fail:user:${usernameUnknown}`,
        `${keyPrefix}:login-fail:ip:${sourceIp}`
    ];

    let tempDirectory;
    let db;
    let redisClient;
    let httpServer;
    let compareCalls = 0;
    let dbPrepareCalls = 0;

    t.after(async () => {
        await closeTestResources({httpServer, redisClient, redisKeys, db, tempDirectory});
    });

    tempDirectory = await mkdtemp(path.join(os.tmpdir(), 'chathub-auth-http-'));
    db = createDatabase(path.join(tempDirectory, 'auth.db'));
    const observedDb = {
        prepare: (...args) => {
            dbPrepareCalls += 1;
            return db.prepare(...args);
        }
    };

    redisClient = createRedisClient({
        url: redisUrl.trim(),
        connectTimeoutMs: 2000,
        maxReconnectAttempts: 0,
        reconnectDelayMs: 0
    });
    await connectRedis(redisClient);

    const limiter = createLoginRateLimiter({
        client: redisClient,
        keyPrefix,
        userLimit: 3,
        ipLimit: 5,
        windowSeconds: 3
    });
    const revocationStore = createJwtRevocationStore({
        client: redisClient,
        key_prefix: keyPrefix
    });
    // 保留真实 bcrypt 行为，同时记录 compare() 是否被登录路由调用。
    const observedBcrypt = {
        hash: (...args) => bcrypt.hash(...args),
        compare: async (...args) => {
            compareCalls += 1;
            return bcrypt.compare(...args);
        }
    };
    const app = createApp({
        db: observedDb,
        limiter,
        bcrypt: observedBcrypt,
        jwt,
        secretKey,
        revocation_store: revocationStore,
        internal_service_key: `http-test-internal-${runId}`
    });
    httpServer = await listenOnLoopback(app);
    const port = httpServer.address().port;
    const baseUrl = `http://127.0.0.1:${port}`;

    assert.ok(Number.isInteger(port) && port > 0);

    const registration = await requestJson(baseUrl, '/register', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    });
    assert.deepEqual(registration, {
        status: 201,
        body: {message: '注册成功'}
    });

    const duplicateRegistration = await requestJson(baseUrl, '/register', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    });
    assert.deepEqual(duplicateRegistration, {
        status: 409,
        body: {error: '用户名已存在'}
    });

    const registrationB = await requestJson(baseUrl, '/register', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username: usernameB, password: passwordB})
    });
    assert.deepEqual(registrationB, {
        status: 201,
        body: {message: '注册成功'}
    });

    const registrationC = await requestJson(baseUrl, '/register', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username: usernameC, password: passwordC})
    });
    assert.deepEqual(registrationC, {
        status: 201,
        body: {message: '注册成功'}
    });

    // R12-2-10：输入合同失败必须在 inspect() 前结束，不触发 SQLite、bcrypt 或失败计数。
    const inputStateBeforeInvalid = await limiter.inspect({username, sourceIp});
    assert.equal(inputStateBeforeInvalid.username.count, 0);
    assert.equal(inputStateBeforeInvalid.username.ttl_seconds, -2);
    assert.equal(inputStateBeforeInvalid.source_ip.count, 0);
    assert.equal(inputStateBeforeInvalid.source_ip.ttl_seconds, -2);
    const compareCallsBeforeInvalid = compareCalls;
    const dbPrepareCallsBeforeInvalid = dbPrepareCalls;

    const invalidLoginCases = [
        {
            body: {username},
            error: '用户名和密码不能为空'
        },
        {
            body: {username: 123, password},
            error: '用户名和密码不能为空'
        },
        {
            body: {username: 'ab', password},
            error: '用户名长度必须在3-20之间'
        },
        {
            body: {username: 'bad-name', password},
            error: '用户名只能包含字母、数字和下划线'
        },
        {
            body: {username, password: '12345'},
            error: '密码长度必须在6-64之间'
        },
        {
            body: {username, password: 'p'.repeat(65)},
            error: '密码长度必须在6-64之间'
        }
    ];

    for (const invalidCase of invalidLoginCases) {
        const invalidLogin = await requestJson(baseUrl, '/login', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(invalidCase.body)
        });
        assert.deepEqual(invalidLogin, {
            status: 400,
            body: {error: invalidCase.error}
        });
    }

    const malformedLogin = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: '{"username":'
    });
    assert.deepEqual(malformedLogin, {
        status: 400,
        body: {error: '请求正文必须是有效 JSON'}
    });

    const inputStateAfterInvalid = await limiter.inspect({username, sourceIp});
    assert.equal(inputStateAfterInvalid.username.count, 0);
    assert.equal(inputStateAfterInvalid.username.ttl_seconds, -2);
    assert.equal(inputStateAfterInvalid.source_ip.count, 0);
    assert.equal(inputStateAfterInvalid.source_ip.ttl_seconds, -2);
    assert.equal(compareCalls, compareCallsBeforeInvalid);
    assert.equal(dbPrepareCalls, dbPrepareCallsBeforeInvalid);

    const failedLogin = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password: 'wrong-password'})
    });
    assert.deepEqual(failedLogin, {
        status: 401,
        body: {error: '用户名或密码错误'}
    });

    const stateAfterFailure = await limiter.inspect({username, sourceIp});
    assert.equal(stateAfterFailure.username.count, 1);
    assert.equal(stateAfterFailure.source_ip.count, 1);

    const successfulLogin = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    });
    assert.equal(successfulLogin.status, 200);
    assert.equal(successfulLogin.body.username, username);
    assert.equal(typeof successfulLogin.body.token, 'string');
    assert.ok(successfulLogin.body.token.length > 0);

    const currentUser = await requestJson(baseUrl, '/me', {
        headers: {Authorization: `Bearer ${successfulLogin.body.token}`}
    });
    assert.deepEqual(currentUser, {
        status: 200,
        body: {username}
    });

    const stateAfterSuccess = await limiter.inspect({username, sourceIp});
    assert.equal(stateAfterSuccess.username.count, 0);
    assert.equal(stateAfterSuccess.username.ttl_seconds, -2);
    assert.equal(stateAfterSuccess.source_ip.count, 1);
    assert.ok(stateAfterSuccess.source_ip.ttl_seconds > 0);

    const firstFailureAfterReset = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password: 'wrong-password'})
    });
    assert.deepEqual(firstFailureAfterReset, {
        status: 401,
        body: {error: '用户名或密码错误'}
    });

    const secondFailureAfterReset = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password: 'wrong-password'})
    });
    assert.deepEqual(secondFailureAfterReset, {
        status: 401,
        body: {error: '用户名或密码错误'}
    });

    const compareCallsBeforeThresholdFailure = compareCalls;
    const thresholdFailure = await requestJsonWithRetryAfter(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password: 'wrong-password'})
    });
    assert.equal(thresholdFailure.status, 429);
    assert.equal(thresholdFailure.body.error, '登录尝试过于频繁，请稍后再试');
    assert.equal(thresholdFailure.body.code, 'login_rate_limited');
    assert.ok(Number.isSafeInteger(thresholdFailure.body.retry_after_seconds));
    assert.ok(thresholdFailure.body.retry_after_seconds > 0);
    assert.equal(
        thresholdFailure.retryAfter,
        String(thresholdFailure.body.retry_after_seconds)
    );
    assert.equal(compareCalls, compareCallsBeforeThresholdFailure + 1);

    const compareCallsBeforeBlockedLogin = compareCalls;
    const blockedCorrectPassword = await requestJsonWithRetryAfter(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    });
    assert.equal(blockedCorrectPassword.status, 429);
    assert.equal(blockedCorrectPassword.body.error, '登录尝试过于频繁，请稍后再试');
    assert.equal(blockedCorrectPassword.body.code, 'login_rate_limited');
    assert.ok(Number.isSafeInteger(blockedCorrectPassword.body.retry_after_seconds));
    assert.ok(blockedCorrectPassword.body.retry_after_seconds > 0);
    assert.equal(
        blockedCorrectPassword.retryAfter,
        String(blockedCorrectPassword.body.retry_after_seconds)
    );
    assert.equal(compareCalls, compareCallsBeforeBlockedLogin);

    const stateAfterBlockedLogin = await limiter.inspect({username, sourceIp});
    assert.equal(stateAfterBlockedLogin.username.count, 3);
    assert.ok(stateAfterBlockedLogin.username.ttl_seconds > 0);
    assert.equal(stateAfterBlockedLogin.source_ip.count, 4);
    assert.ok(stateAfterBlockedLogin.source_ip.ttl_seconds > 0);

    const userBLogin = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username: usernameB, password: passwordB})
    });
    assert.equal(userBLogin.status, 200);
    assert.equal(userBLogin.body.username, usernameB);
    assert.equal(typeof userBLogin.body.token, 'string');
    assert.ok(userBLogin.body.token.length > 0);

    const stateAfterUserBLogin = await limiter.inspect({
        username: usernameB,
        sourceIp
    });
    assert.equal(stateAfterUserBLogin.username.count, 0);
    assert.equal(stateAfterUserBLogin.username.ttl_seconds, -2);
    assert.equal(stateAfterUserBLogin.source_ip.count, 4);
    assert.ok(stateAfterUserBLogin.source_ip.ttl_seconds > 0);
    assert.equal(stateAfterUserBLogin.limited, false);

    const stateAfterUserBLoginForA = await limiter.inspect({username, sourceIp});
    assert.equal(stateAfterUserBLoginForA.username.count, 3);
    assert.ok(stateAfterUserBLoginForA.username.ttl_seconds > 0);
    assert.equal(stateAfterUserBLoginForA.source_ip.count, 4);
    assert.equal(stateAfterUserBLoginForA.limited, true);

    const compareCallsBeforeUserABlockedAgain = compareCalls;
    const userABlockedAgain = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    });
    assert.equal(userABlockedAgain.status, 429);
    assert.equal(userABlockedAgain.body.code, 'login_rate_limited');
    assert.equal(compareCalls, compareCallsBeforeUserABlockedAgain);

    const compareCallsBeforeIpFailure = compareCalls;
    const userBIpFailure = await requestJsonWithRetryAfter(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username: usernameB, password: 'wrong-password'})
    });
    assert.equal(userBIpFailure.status, 429);
    assert.equal(userBIpFailure.body.code, 'login_rate_limited');
    assert.ok(Number.isSafeInteger(userBIpFailure.body.retry_after_seconds));
    assert.ok(userBIpFailure.body.retry_after_seconds > 0);
    assert.equal(
        userBIpFailure.retryAfter,
        String(userBIpFailure.body.retry_after_seconds)
    );
    assert.equal(compareCalls, compareCallsBeforeIpFailure + 1);

    const stateAfterIpFailure = await limiter.inspect({username: usernameB, sourceIp});
    assert.equal(stateAfterIpFailure.username.count, 1);
    assert.ok(stateAfterIpFailure.username.ttl_seconds > 0);
    assert.equal(stateAfterIpFailure.source_ip.count, 5);
    assert.ok(stateAfterIpFailure.source_ip.ttl_seconds > 0);
    assert.equal(stateAfterIpFailure.limited, true);

    const compareCallsBeforeNewUserBlocked = compareCalls;
    const newUserBlockedByIp = await requestJsonWithRetryAfter(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username: usernameC, password: passwordC})
    });
    assert.equal(newUserBlockedByIp.status, 429);
    assert.equal(newUserBlockedByIp.body.code, 'login_rate_limited');
    assert.ok(Number.isSafeInteger(newUserBlockedByIp.body.retry_after_seconds));
    assert.ok(newUserBlockedByIp.body.retry_after_seconds > 0);
    assert.equal(
        newUserBlockedByIp.retryAfter,
        String(newUserBlockedByIp.body.retry_after_seconds)
    );
    assert.equal(compareCalls, compareCallsBeforeNewUserBlocked);

    const stateAfterNewUserBlocked = await limiter.inspect({username: usernameC, sourceIp});
    assert.equal(stateAfterNewUserBlocked.username.count, 0);
    assert.equal(stateAfterNewUserBlocked.username.ttl_seconds, -2);
    assert.equal(stateAfterNewUserBlocked.source_ip.count, 5);
    assert.ok(stateAfterNewUserBlocked.source_ip.ttl_seconds > 0);
    assert.equal(stateAfterNewUserBlocked.limited, true);

    const expiredState = await waitForNoFailures(limiter, {username, sourceIp});
    assert.equal(expiredState.username.count, 0);
    assert.equal(expiredState.username.ttl_seconds, -2);
    assert.equal(expiredState.source_ip.count, 0);
    assert.equal(expiredState.source_ip.ttl_seconds, -2);
    assert.equal(expiredState.limited, false);

    const compareCallsBeforeRecovery = compareCalls;
    const recoveredLogin = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    });
    assert.equal(recoveredLogin.status, 200);
    assert.equal(recoveredLogin.body.username, username);
    assert.equal(typeof recoveredLogin.body.token, 'string');
    assert.ok(recoveredLogin.body.token.length > 0);
    assert.equal(compareCalls, compareCallsBeforeRecovery + 1);

    const compareCallsBeforeUnknownUser = compareCalls;
    const unknownUserLogin = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            username: usernameUnknown,
            password: 'password000'
        })
    });
    assert.deepEqual(unknownUserLogin, {
        status: 401,
        body: {error: '用户名或密码错误'}
    });
    assert.equal(compareCalls, compareCallsBeforeUnknownUser);

    const stateAfterUnknownUser = await limiter.inspect({
        username: usernameUnknown,
        sourceIp
    });
    assert.equal(stateAfterUnknownUser.username.count, 1);
    assert.ok(stateAfterUnknownUser.username.ttl_seconds > 0);
    assert.equal(stateAfterUnknownUser.source_ip.count, 1);
    assert.ok(stateAfterUnknownUser.source_ip.ttl_seconds > 0);
    assert.equal(stateAfterUnknownUser.limited, false);

    const compareCallsBeforeKnownWrongPassword = compareCalls;
    const knownWrongPassword = await requestJson(baseUrl, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password: 'wrong-password'})
    });
    assert.deepEqual(knownWrongPassword, unknownUserLogin);
    assert.equal(compareCalls, compareCallsBeforeKnownWrongPassword + 1);
});

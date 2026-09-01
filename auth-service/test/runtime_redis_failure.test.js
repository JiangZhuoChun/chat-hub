'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {mkdtemp, rm} = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const {createApp} = require('../src/app');
const {createDatabase} = require('../src/db');
const {closeHttpServer} = require('../src/server');
const {requestJson} = require('./http_test_helpers');

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

function loginRequest(username, password) {
    return {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    };
}

function makeLimiter(calls) {
    let mode = 'failed';
    const limiter = {
        setReady() {
            mode = 'ready';
        },
        setRecordFailureFailed() {
            mode = 'record_failure_failed';
        },
        setClearFailed() {
            mode = 'clear_failed';
        },
        async inspect() {
            calls.inspect += 1;
            if (mode === 'failed') {
                const error = new Error('runtime_redis_disconnected');
                error.code = 'redis_unavailable';
                throw error;
            }
            return {
                username: {count: 0, ttl_seconds: -2, limited: false},
                source_ip: {count: 0, ttl_seconds: -2, limited: false},
                limited: false,
                retry_after_seconds: 0
            };
        },
        async recordFailure() {
            calls.record_failure += 1;
            if (mode === 'record_failure_failed') {
                const error = new Error('runtime_redis_disconnected');
                error.code = 'redis_unavailable';
                throw error;
            }
            return {
                username: {count: 1, ttl_seconds: 60, limited: false},
                source_ip: {count: 1, ttl_seconds: 60, limited: false},
                limited: false,
                retry_after_seconds: 0
            };
        },
        async clearUserFailures() {
            calls.clear_user_failures += 1;
            if (mode === 'clear_failed') {
                const error = new Error('runtime_redis_disconnected');
                error.code = 'redis_unavailable';
                throw error;
            }
            return {deleted: 1};
        }
    };
    return limiter;
}

test('当 Redis 分别在 inspect、recordFailure、clearUserFailures 故障时，登录应在各门禁返回 503', {timeout: 15000}, async t => {
    const calls = {
        inspect: 0,
        record_failure: 0,
        clear_user_failures: 0,
        db_prepare: 0,
        bcrypt_compare: 0,
        jwt_sign: 0
    };
    const limiter = makeLimiter(calls);
    const db = {
        prepare() {
            calls.db_prepare += 1;
            return {
                get() {
                    return {username: 'alice', password: 'stored-hash'};
                }
            };
        }
    };
    const bcrypt = {
        async hash() {
            return 'stored-hash';
        },
        async compare(password) {
            calls.bcrypt_compare += 1;
            return password !== 'wrong-password';
        }
    };
    const jwt = {
        sign() {
            calls.jwt_sign += 1;
            return 'test-token';
        },
        verify() {
            return {username: 'alice', jti: 'jti', exp: Math.floor(Date.now() / 1000) + 60};
        }
    };
    const revocation_store = {
        async isRevoked() {
            return false;
        },
        async revoke() {
            return {revoked: true, expired: false, created: true, ttl_seconds: 60};
        }
    };
    const temp_directory = await mkdtemp(path.join(os.tmpdir(), 'chathub-auth-runtime-'));
    const database = createDatabase(path.join(temp_directory, 'auth.db'));
    const app = createApp({
        db,
        limiter,
        bcrypt,
        jwt,
        secretKey: 'runtime-test-secret',
        revocation_store,
        internal_service_key: 'runtime-internal-key'
    });
    const http_server = await listenOnLoopback(app);
    const base_url = `http://127.0.0.1:${http_server.address().port}`;
    t.after(async () => {
        await closeHttpServer(http_server);
        database.close();
        await rm(temp_directory, {recursive: true, force: true});
    });

    const inspect_failure = await requestJson(
        base_url,
        '/login',
        loginRequest('alice', 'password123')
    );
    assert.deepEqual(inspect_failure, {
        status: 503,
        body: {
            error: '认证服务暂时不可用',
            code: 'authentication_dependency_unavailable'
        }
    });
    assert.equal(calls.inspect, 1);
    assert.equal(calls.db_prepare, 0);
    assert.equal(calls.bcrypt_compare, 0);
    assert.equal(calls.jwt_sign, 0);

    limiter.setReady();
    limiter.setRecordFailureFailed();
    const record_failure_failure = await requestJson(
        base_url,
        '/login',
        loginRequest('alice', 'wrong-password')
    );
    assert.deepEqual(record_failure_failure, {
        status: 503,
        body: {
            error: '认证服务暂时不可用',
            code: 'authentication_dependency_unavailable'
        }
    });
    assert.equal(calls.record_failure, 1);
    assert.equal(calls.jwt_sign, 0);

    limiter.setReady();
    limiter.setClearFailed();
    const clear_failure = await requestJson(
        base_url,
        '/login',
        loginRequest('alice', 'password123')
    );
    assert.deepEqual(clear_failure, {
        status: 503,
        body: {
            error: '认证服务暂时不可用',
            code: 'authentication_dependency_unavailable'
        }
    });
    assert.equal(calls.clear_user_failures, 1);
    assert.equal(calls.jwt_sign, 0);
});

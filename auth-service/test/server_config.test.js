'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const {parseConfig} = require('../src/server');

function baseEnvironment() {
    return {
        CHATHUB_REDIS_URL: 'redis://127.0.0.1:6379',
        SECRET_KEY: 'config-test-secret',
        CHATHUB_AUTH_INTERNAL_SERVICE_KEY: 'config-test-internal-key'
    };
}

test('当 Auth 内部服务密钥缺失时，配置解析应拒绝启动', () => {
    const environment = baseEnvironment();
    delete environment.CHATHUB_AUTH_INTERNAL_SERVICE_KEY;

    assert.throws(
        () => parseConfig(environment),
        error => error.code === 'auth_startup_config_invalid'
            && error.message === 'CHATHUB_AUTH_INTERNAL_SERVICE_KEY_missing'
    );
});

test('当环境配置包含可清理的文本值时，配置解析应返回规范化副本', () => {
    const environment = {
        ...baseEnvironment(),
        CHATHUB_REDIS_URL: ' redis://127.0.0.1:6379 ',
        SECRET_KEY: ' config-test-secret ',
        CHATHUB_AUTH_INTERNAL_SERVICE_KEY: ' config-test-internal-key ',
        CHATHUB_REDIS_KEY_PREFIX: 'chathub:test:config',
        CHATHUB_LOGIN_USER_LIMIT: '3',
        CHATHUB_LOGIN_IP_LIMIT: '5',
        CHATHUB_LOGIN_WINDOW_SECONDS: '60'
    };

    assert.deepEqual(parseConfig(environment), {
        redisUrl: 'redis://127.0.0.1:6379',
        secretKey: 'config-test-secret',
        internal_service_key: 'config-test-internal-key',
        keyPrefix: 'chathub:test:config',
        userLimit: 3,
        ipLimit: 5,
        windowSeconds: 60,
        port: 3000
    });
});

test('当限流参数不是正整数或 IP 限额过低时，配置解析应拒绝启动', () => {
    const invalid_window = {
        ...baseEnvironment(),
        CHATHUB_LOGIN_WINDOW_SECONDS: '0'
    };
    assert.throws(
        () => parseConfig(invalid_window),
        error => error.code === 'auth_startup_config_invalid'
            && error.message === 'CHATHUB_LOGIN_WINDOW_SECONDS_invalid'
    );

    const invalid_relation = {
        ...baseEnvironment(),
        CHATHUB_LOGIN_USER_LIMIT: '6',
        CHATHUB_LOGIN_IP_LIMIT: '5'
    };
    assert.throws(
        () => parseConfig(invalid_relation),
        error => error.code === 'auth_startup_config_invalid'
            && error.message === 'CHATHUB_LOGIN_IP_LIMIT_below_user_limit'
    );
});

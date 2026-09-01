'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {randomUUID} = require('node:crypto');

const {
    createRedisClient,
    connectRedis,
    closeRedis
} = require('../src/redis_client');
const {createLoginRateLimiter} = require('../src/login_rate_limiter');

const redisUrl = process.env.CHATHUB_REDIS_TEST_URL;
if (typeof redisUrl !== 'string' || redisUrl.trim().length === 0) {
    throw new Error('CHATHUB_REDIS_TEST_URL_missing');
}

// 功能：在有界截止时间内确认并发失败产生的两个 key 都已过期，避免永久等待。
async function waitForLimiterExpiry(limiter, identity, {
    timeoutMs = 7000,
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

    throw new Error('concurrent_login_failure_expiry_timeout');
}

test('当首次失败建立固定窗口时，后续失败应只增加计数而不重置 TTL', {timeout: 15000}, async t => {
    const runId = randomUUID().replace(/-/g, '');
    const username = `redis_test_${runId.slice(0, 8)}`;
    const sourceIp = '127.0.0.1';
    const keyPrefix = `chathub:test:${runId}`;
    const userKey = `${keyPrefix}:login-fail:user:${username}`;
    const sourceIpKey = `${keyPrefix}:login-fail:ip:${sourceIp}`;

    const client = createRedisClient({
        url: redisUrl.trim(),
        connectTimeoutMs: 2000,
        maxReconnectAttempts: 0,
        reconnectDelayMs: 0
    });

    t.after(async () => {
        if (client.isOpen && client.isReady) {
            await client.del(userKey, sourceIpKey);
        }
        await closeRedis(client);
    });

    await connectRedis(client);
    const limiter = createLoginRateLimiter({
        client,
        keyPrefix,
        userLimit: 2,
        ipLimit: 3,
        windowSeconds: 30
    });

    const initial = await limiter.inspect({username, sourceIp});
    assert.equal(initial.limited, false);
    assert.equal(initial.username.count, 0);
    assert.equal(initial.username.ttl_seconds, -2);
    assert.equal(initial.source_ip.count, 0);
    assert.equal(initial.source_ip.ttl_seconds, -2);

    const first = await limiter.recordFailure({username, sourceIp});
    assert.equal(first.username.count, 1);
    assert.equal(first.username.expiry_set, 1);
    assert.ok(first.username.ttl_seconds > 0);
    assert.equal(first.source_ip.count, 1);
    assert.equal(first.source_ip.expiry_set, 1);
    assert.ok(first.source_ip.ttl_seconds > 0);
    assert.equal(first.limited, false);

    const second = await limiter.recordFailure({username, sourceIp});
    assert.equal(second.username.count, 2);
    assert.equal(second.username.expiry_set, 0);
    assert.ok(second.username.ttl_seconds > 0);
    assert.equal(second.source_ip.count, 2);
    assert.equal(second.source_ip.expiry_set, 0);
    assert.ok(second.source_ip.ttl_seconds > 0);
    assert.equal(second.limited, true);
    assert.ok(second.retry_after_seconds >= 1);

    const inspectedLimited = await limiter.inspect({username, sourceIp});
    assert.equal(inspectedLimited.limited, true);
    assert.equal(inspectedLimited.username.count, 2);

    assert.deepEqual(
        await limiter.clearUserFailures(username),
        {deleted: 1}
    );

    const afterClear = await limiter.inspect({username, sourceIp});
    assert.equal(afterClear.username.count, 0);
    assert.equal(afterClear.username.ttl_seconds, -2);
    assert.equal(afterClear.source_ip.count, 2);
    assert.ok(afterClear.source_ip.ttl_seconds > 0);
});

test('当并发记录登录失败时，计数应精确且固定窗口不延长', {timeout: 20000}, async t => {
    const runId = randomUUID().replace(/-/g, '');
    const username = `rc_${runId.slice(0, 8)}`;
    const sourceIp = '127.0.0.1';
    const keyPrefix = `chathub:test:concurrent:${runId}`;
    const userKey = `${keyPrefix}:login-fail:user:${username}`;
    const sourceIpKey = `${keyPrefix}:login-fail:ip:${sourceIp}`;
    const windowSeconds = 5;
    const concurrency = 20;

    const client = createRedisClient({
        url: redisUrl.trim(),
        connectTimeoutMs: 2000,
        maxReconnectAttempts: 0,
        reconnectDelayMs: 0
    });

    t.after(async () => {
        if (client.isOpen && client.isReady) {
            await client.del(userKey, sourceIpKey);
        }
        await closeRedis(client);
    });

    await connectRedis(client);
    const limiter = createLoginRateLimiter({
        client,
        keyPrefix,
        userLimit: 100,
        ipLimit: 100,
        windowSeconds
    });

    const firstWave = await Promise.all(
        Array.from({length: concurrency}, () => (
            limiter.recordFailure({username, sourceIp})
        ))
    );

    const expectedFirstWaveCounts = Array.from(
        {length: concurrency},
        (_value, index) => index + 1
    );
    assert.deepEqual(
        firstWave
            .map(result => result.username.count)
            .sort((left, right) => left - right),
        expectedFirstWaveCounts
    );
    assert.deepEqual(
        firstWave
            .map(result => result.source_ip.count)
            .sort((left, right) => left - right),
        expectedFirstWaveCounts
    );
    assert.equal(
        firstWave.filter(result => result.username.expiry_set === 1).length,
        1
    );
    assert.equal(
        firstWave.filter(result => result.source_ip.expiry_set === 1).length,
        1
    );

    const firstWaveState = await limiter.inspect({username, sourceIp});
    assert.equal(firstWaveState.username.count, concurrency);
    assert.equal(firstWaveState.source_ip.count, concurrency);
    assert.ok(firstWaveState.username.ttl_seconds > 0);
    assert.ok(firstWaveState.username.ttl_seconds <= windowSeconds);
    assert.ok(firstWaveState.source_ip.ttl_seconds > 0);
    assert.ok(firstWaveState.source_ip.ttl_seconds <= windowSeconds);

    await new Promise(resolve => {
        setTimeout(resolve, 1100);
    });

    const delayedFailure = await limiter.recordFailure({username, sourceIp});
    assert.equal(delayedFailure.username.count, concurrency + 1);
    assert.equal(delayedFailure.source_ip.count, concurrency + 1);
    assert.equal(delayedFailure.username.expiry_set, 0);
    assert.equal(delayedFailure.source_ip.expiry_set, 0);
    assert.ok(delayedFailure.username.ttl_seconds > 0);
    assert.ok(delayedFailure.username.ttl_seconds < windowSeconds);
    assert.ok(delayedFailure.source_ip.ttl_seconds > 0);
    assert.ok(delayedFailure.source_ip.ttl_seconds < windowSeconds);

    const expiredState = await waitForLimiterExpiry(
        limiter,
        {username, sourceIp}
    );
    assert.equal(expiredState.username.count, 0);
    assert.equal(expiredState.username.ttl_seconds, -2);
    assert.equal(expiredState.source_ip.count, 0);
    assert.equal(expiredState.source_ip.ttl_seconds, -2);
    assert.equal(expiredState.limited, false);
});

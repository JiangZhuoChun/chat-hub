'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const {withDeadline} = require('./e2e_deadline');

test('deadline 内完成时保留原 Promise 的结果', async () => {
    const result = await withDeadline(Promise.resolve({status: 'ok'}), 100, 'demo');

    assert.deepEqual(result, {status: 'ok'});
});

test('超过 deadline 时返回稳定 timeout 错误', async () => {
    const never_settles = new Promise(() => {});

    await assert.rejects(
        () => withDeadline(never_settles, 20, 'demo'),
        error => error instanceof Error && error.message === 'demo_timeout'
    );
});

test('真实操作先失败时保留原错误而不是伪装为 timeout', async () => {
    const expected_error = new Error('connect_refused');

    await assert.rejects(
        () => withDeadline(Promise.reject(expected_error), 100, 'demo'),
        error => error === expected_error
    );
});

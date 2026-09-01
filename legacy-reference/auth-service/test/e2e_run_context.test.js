'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {existsSync} = require('node:fs');

const {createE2eRunContext} = require('./e2e_run_context');

test('每次运行使用不同的临时目录和 Redis 前缀，并在 cleanup 后删除目录', async () => {
    const first = await createE2eRunContext();
    const second = await createE2eRunContext();

    try {
        assert.notEqual(first.run_id, second.run_id);
        assert.notEqual(first.temporary_directory, second.temporary_directory);
        assert.notEqual(first.redis_key_prefix, second.redis_key_prefix);
        assert.equal(existsSync(first.temporary_directory), true);
        assert.equal(existsSync(second.temporary_directory), true);
    } finally {
        // 即使断言失败也回收本次临时目录，避免测试失败时泄漏资源。
        await first.cleanup();
        await second.cleanup();
    }

    assert.equal(existsSync(first.temporary_directory), false);
    assert.equal(existsSync(second.temporary_directory), false);
});

test('cleanup 按登记的反向顺序释放资源', async () => {
    const context = await createE2eRunContext();
    const cleanup_order = [];

    try {
        context.defer(async () => cleanup_order.push('socket'));
        context.defer(async () => cleanup_order.push('chat_server_process'));
        context.defer(async () => cleanup_order.push('auth_process'));

        await context.cleanup();

        assert.deepEqual(cleanup_order, [
            'auth_process',
            'chat_server_process',
            'socket'
        ]);
        assert.equal(existsSync(context.temporary_directory), false);
    } finally {
        await context.cleanup();
    }
});

test('某个清理动作失败时仍继续回收其余资源和临时目录', async () => {
    const context = await createE2eRunContext();
    const cleanup_order = [];
    const expected_error = new Error('socket_close_failed');

    try {
        context.defer(async () => cleanup_order.push('first'));
        context.defer(async () => {
            cleanup_order.push('failed');
            throw expected_error;
        });
        context.defer(async () => cleanup_order.push('last'));

        await assert.rejects(() => context.cleanup(), error => error === expected_error);

        assert.deepEqual(cleanup_order, ['last', 'failed', 'first']);
        assert.equal(existsSync(context.temporary_directory), false);
    } finally {
        // cleanup() 返回同一个已完成 Promise；此处保证未来改动也不会漏掉释放入口。
        await context.cleanup().catch(() => {});
    }
});

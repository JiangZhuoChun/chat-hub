'use strict';

const {randomUUID} = require('node:crypto');
const {mkdtemp, rm} = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

// 为一次 E2E/负载运行创建独立身份和临时目录，避免误用其他测试留下的状态。
async function createE2eRunContext() {
    const run_id = randomUUID();
    const temporary_directory = await mkdtemp(
        path.join(os.tmpdir(), 'chathub-w13-load-')
    );
    const cleanup_actions = [];
    let cleanup_promise = null;

    const run_context = {
        run_id,
        temporary_directory,
        // 后续只允许清理此前缀的 key；它不是 Redis 地址或任何秘密。
        redis_key_prefix: `chathub:w13:load:${run_id}:`,

        defer(cleanup_action) {
            if (cleanup_promise) {
                throw new Error('e2e_run_context_closed');
            }
            if (typeof cleanup_action !== 'function') {
                throw new TypeError('e2e_cleanup_action_invalid');
            }
            cleanup_actions.push(cleanup_action);
        },

        cleanup() {
            if (!cleanup_promise) {
                cleanup_promise = cleanupRunContext();
            }
            return cleanup_promise;
        }
    };

    async function cleanupRunContext() {
        const failures = [];

        // 资源创建顺序的反向清理：socket/子进程先于临时目录释放。
        while (cleanup_actions.length > 0) {
            const cleanup_action = cleanup_actions.pop();
            try {
                await cleanup_action();
            } catch (error) {
                // 一个资源清理失败也不能阻止其余资源继续回收。
                failures.push(error);
            }
        }

        try {
            await rm(temporary_directory, {recursive: true, force: true});
        } catch (error) {
            failures.push(error);
        }

        if (failures.length === 1) {
            throw failures[0];
        }
        if (failures.length > 1) {
            throw new AggregateError(failures, 'e2e_run_context_cleanup_failed');
        }
    }

    return run_context;
}

module.exports = {createE2eRunContext};
